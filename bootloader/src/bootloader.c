/**
 * @file bootloader.c
 * @author Kyle Scaplen
 * @brief Bootloader implementation
 * @date 2022
 * 
 * This source file is part of an example system for MITRE's 2022 Embedded System CTF (eCTF).
 * This code is being provided only for educational purposes for the 2022 MITRE eCTF competition,
 * and may not meet MITRE standards for quality. Use this code at your own risk!
 * 
 * @copyright Copyright (c) 2022 The MITRE Corporation
 */

#include <stdint.h>
#include <stdbool.h>

#include "driverlib/interrupt.h"

#include "flash.h"
#include "uart.h"

// this will run if EXAMPLE_AES is defined in the Makefile (see line 54)
#ifdef EXAMPLE_AES
#include "aes.h"
#include "bearssl_hash.h"
#include "bearssl_rsa.h"
#include "bearssl_block.h"
#include "inner.h"
#endif


// Storage layout

/*
 * Firmware:
 *      Hash:    0x0002B3B0 : 0x0002B400 (50B = 32B + pad)
 *      Size:    0x0002B400 : 0x0002B404 (4B)
 *      Version: 0x0002B404 : 0x0002B408 (4B)
 *      Msg:     0x0002B408 : 0x0002BC00 (~2KB = 1KB + 1B + pad)
 *      Fw:      0x0002BC00 : 0x0002FC00 (16KB)
 * Configuration:
 *      Size:    0x0002FC00 : 0x0003000 (1KB = 4B + pad)
 *      Cfg:     0x00030000 : 0x0004000 (64KB)
 */
#define FIRMWARE_AES_PTR           ((uint32_t)(FLASH_START + 0x0002B370))
#define FIRMWARE_HASH_PTR          ((unsigned char *)(FLASH_START + 0x0002B3B0))
#define FIRMWARE_METADATA_PTR      ((uint32_t)(FLASH_START + 0x0002B400))
#define FIRMWARE_SIZE_PTR          ((uint32_t)(FIRMWARE_METADATA_PTR + 0))
#define FIRMWARE_VERSION_PTR       ((uint32_t)(FIRMWARE_METADATA_PTR + 4))
#define FIRMWARE_RELEASE_MSG_PTR   ((uint32_t)(FIRMWARE_METADATA_PTR + 8))
#define FIRMWARE_RELEASE_MSG_PTR2  ((uint32_t)(FIRMWARE_METADATA_PTR + FLASH_PAGE_SIZE))
#define FIRMWARE_DATA_PTR          ((unsigned char)(FIRMWARE_METADATA_PTR + (FLASH_PAGE_SIZE*2)))
#define FIRMWARE_STORAGE_PTR       ((uint32_t)(FIRMWARE_METADATA_PTR + (FLASH_PAGE_SIZE*2)))
#define FIRMWARE_BOOT_PTR          ((uint32_t)0x20004000)

#define CONFIGURATION_METADATA_PTR ((uint32_t)(FIRMWARE_STORAGE_PTR + (FLASH_PAGE_SIZE*16)))
#define CONFIGURATION_SIZE_PTR     ((uint32_t)(CONFIGURATION_METADATA_PTR + 0))

#define CONFIGURATION_STORAGE_PTR  ((uint32_t)(CONFIGURATION_METADATA_PTR + FLASH_PAGE_SIZE))




// Firmware update constants
#define FRAME_OK 0x00
#define FRAME_BAD 0x01

static unsigned char aes_key[16] = {
    0x1a, 0x2a, 0x3a, 0x4a, 0x5a, 0x6a, 0x7a, 0x8a,
    0x1a, 0x2a, 0x3a, 0x4a, 0x5a, 0x6a, 0x7a, 0x8a
};


/**
 * @brief Boot the firmware.
 */
void handle_boot(void)
{
    uint32_t size;
    uint32_t i = 0;
    uint8_t *rel_msg;

    // Acknowledge the host
    uart_writeb(HOST_UART, 'B');

    // Find the metadata
    size = *((uint32_t *)FIRMWARE_SIZE_PTR);

    // Copy the firmware into the Boot RAM section
    for (i = 0; i < size; i++) {
        *((uint8_t *)(FIRMWARE_BOOT_PTR + i)) = *((uint8_t *)(FIRMWARE_STORAGE_PTR + i));
    }

    uart_writeb(HOST_UART, 'M');

    // Print the release message
    rel_msg = (uint8_t *)FIRMWARE_RELEASE_MSG_PTR;
    while (*rel_msg != 0) {
        uart_writeb(HOST_UART, *rel_msg);
        rel_msg++;
    }
    uart_writeb(HOST_UART, '\0');

    // Execute the firmware
    void (*firmware)(void) = (void (*)(void))(FIRMWARE_BOOT_PTR + 1);
    firmware();
}


/**
 * @brief Send the firmware data over the host interface.
 */
void handle_readback(void)
{
    uint8_t region;
    uint8_t *address;
    uint32_t size = 0;
    
    // Acknowledge the host
    uart_writeb(HOST_UART, 'R');

    // Receive region identifier
    region = (uint32_t)uart_readb(HOST_UART);

    if (region == 'F') {
        // Set the base address for the readback
        address = (uint8_t *)FIRMWARE_STORAGE_PTR;
        // Acknowledge the host
        uart_writeb(HOST_UART, 'F');
    } else if (region == 'C') {
        // Set the base address for the readback
        address = (uint8_t *)CONFIGURATION_STORAGE_PTR;
        // Acknowledge the hose
        uart_writeb(HOST_UART, 'C');
    } else {
        return;
    }

    // Receive the size to send back to the host
    size = ((uint32_t)uart_readb(HOST_UART)) << 24;
    size |= ((uint32_t)uart_readb(HOST_UART)) << 16;
    size |= ((uint32_t)uart_readb(HOST_UART)) << 8;
    size |= (uint32_t)uart_readb(HOST_UART);

    // Read out the memory
    uart_write(HOST_UART, address, size);
}


/**
 * @brief Read data from a UART interface and program to flash memory.
 * 
 * @param interface is the base address of the UART interface to read from.
 * @param dst is the starting page address to store the data.
 * @param size is the number of bytes to load.
 */
void load_data(uint32_t interface, uint32_t dst, uint32_t size)
{
    int i;
    uint32_t frame_size;
    uint8_t page_buffer[FLASH_PAGE_SIZE];

    while(size > 0) {
        // calculate frame size
        frame_size = size > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : size;
        // read frame into buffer
        uart_read(HOST_UART, page_buffer, frame_size);
        // pad buffer if frame is smaller than the page
        for(i = frame_size; i < FLASH_PAGE_SIZE; i++) {
            page_buffer[i] = 0xFF;
        }
        // clear flash page
        flash_erase_page(dst);
        // write flash page
        flash_write((uint32_t *)page_buffer, dst, FLASH_PAGE_SIZE >> 2);
        // next page and decrease size
        dst += FLASH_PAGE_SIZE;
        size -= frame_size;
        // send frame ok
        uart_writeb(HOST_UART, FRAME_OK);
    }
}

static void compute_sha256(const void *data, int len, void *out)
{
    br_sha256_context csha256;

    br_sha256_init(&csha256);
    br_sha256_update(&csha256, data, len);
    br_sha256_out(&csha256, out);

    return;
}

static void do_AES_decrypt(char *enc_data, int len,
    const br_block_cbcenc_class *ve,
    const br_block_cbcdec_class *vd)
{
    static unsigned char key[32]; //16
    static unsigned char iv[16];
    size_t key_len;
    br_aes_gen_cbcdec_keys v_dc;
    const br_block_cbcdec_class **dc;

    if (ve->block_size != 16 || vd->block_size != 16
            || ve->log_block_size != 4 || vd->log_block_size != 4)
    {
        return;
    }

    dc = &v_dc.vtable;
    key_len = sizeof(aes_key);
    memcpy(key, aes_key, sizeof(aes_key));
#if 1
    vd->init(dc, key, key_len);
    memset(iv, 0, sizeof iv);
    vd->run(dc, iv, enc_data, len);
#endif
}

/**
 * @brief Decrypt firmware
 * 
 * @return : 0 on success, -1 on failure.
 */

int decrypt_firmware(unsigned char *image, unsigned int totalsize)
{
    unsigned int image_size = 0;
    static unsigned char hash[32];
    static unsigned char decrypted_hash[32];
    int i = 0;

    if (NULL == image)
    {
        return -1;
    }

    // Get the actual size of the software image
    memcpy(&image_size, &image[FIRMWARE_SIZE_PTR], 4);
    /*g_ui32ImageSize = image_size;

    // Decrypt the encrypted AES key & store it
    if (!br_rsa_i15_private((unsigned char *)&image[AES_KEY_OFFSET], &RSA_SK)) {
        ERROR( "RSA decryption of AES key failed (1)\r");
        return 1;
    }
    memcpy(aes_key, &image[AES_KEY_OFFSET], sizeof(aes_key));
     


    if (!br_rsa_i15_pkcs1_vrfy((unsigned char *)&image[SHA256_OFFSET], SHA256_SIZE, BR_HASH_OID_SHA256, \
                sizeof (decrypted_hash), &RSA_PK, decrypted_hash))
    {
        ERROR("Signature verification failed (2)\r");
        return -1;
    }
    */
    // Decrypt the application with the AES key 
   do_AES_decrypt(FIRMWARE_DATA_PTR  , image_size,
        &br_aes_big_cbcenc_vtable,
        &br_aes_big_cbcdec_vtable);

    // Perform SHA256 sum check and store the result on `hash`
    compute_sha256(FIRMWARE_DATA_PTR, image_size, hash);

    // check if hash== FIRMWARE_HASH_PTR
    for(i=0; i<32; i++){
        if (hash[i] != FIRMWARE_HASH_PTR[i]){
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Update the firmware.
 */
void handle_update(void)
{
    // metadata
    uint32_t current_version;
    uint32_t version = 0;
    uint32_t size = 0;
    uint32_t rel_msg_size = 0;
    uint8_t rel_msg[1025]; // 1024 + terminator
    uint8_t sha256_hash[65]; // 64 + terminator
    uint8_t sha256_size = 0;
    uint8_t ret = 0;

    // Acknowledge the host
    uart_writeb(HOST_UART, 'U');

    // Receive version
    version = ((uint32_t)uart_readb(HOST_UART)) << 8;
    version |= (uint32_t)uart_readb(HOST_UART);

    // Receive size
    size = ((uint32_t)uart_readb(HOST_UART)) << 24;
    size |= ((uint32_t)uart_readb(HOST_UART)) << 16;
    size |= ((uint32_t)uart_readb(HOST_UART)) << 8;
    size |= (uint32_t)uart_readb(HOST_UART);

    // Receive release message
    rel_msg_size = uart_readline(HOST_UART, rel_msg) + 1; // Include terminator

    // Recieve SHA 256 hash
    sha256_size = uart_readline(HOST_UART, sha256_hash) + 1; // Include terminator

    // Check the version
    current_version = *((uint32_t *)FIRMWARE_VERSION_PTR);
    if (current_version == 0xFFFFFFFF) {
        current_version = (uint32_t)OLDEST_VERSION;
    }

    if ((version != 0) && (version < current_version)) {
        // Version is not acceptable
        uart_writeb(HOST_UART, FRAME_BAD);
        return;
    }

    // Clear firmware metadata
    flash_erase_page(FIRMWARE_METADATA_PTR);

    // Only save new version if it is not 0
    if (version != 0) {
        flash_write_word(version, FIRMWARE_VERSION_PTR);
    } else {
        flash_write_word(current_version, FIRMWARE_VERSION_PTR);
    }

    // Save size
    flash_write_word(size, FIRMWARE_SIZE_PTR);

    // Write release message
    uint8_t *rel_msg_read_ptr = rel_msg;
    uint32_t rel_msg_write_ptr = FIRMWARE_RELEASE_MSG_PTR;
    uint32_t rem_bytes = rel_msg_size;

    // If release message goes outside of the first page, write the first full page
    if (rel_msg_size > (FLASH_PAGE_SIZE-8)) {

        // Write first page
        flash_write((uint32_t *)rel_msg, FIRMWARE_RELEASE_MSG_PTR, (FLASH_PAGE_SIZE-8) >> 2); // This is always a multiple of 4

        // Set up second page
        rem_bytes = rel_msg_size - (FLASH_PAGE_SIZE-8);
        rel_msg_read_ptr = rel_msg + (FLASH_PAGE_SIZE-8);
        rel_msg_write_ptr = FIRMWARE_RELEASE_MSG_PTR2;
        flash_erase_page(rel_msg_write_ptr);
    }

    // Program last or only page of release message
    if (rem_bytes % 4 != 0) {
        rem_bytes += 4 - (rem_bytes % 4); // Account for partial word
    }
    flash_write((uint32_t *)rel_msg_read_ptr, rel_msg_write_ptr, rem_bytes >> 2);

    // Acknowledge
    uart_writeb(HOST_UART, FRAME_OK);
    
    // Retrieve firmware
    load_data(HOST_UART, FIRMWARE_STORAGE_PTR, size);

    ret = decrypt_firmware(FIRMWARE_BOOT_PTR, FIRMWARE_SIZE_PTR);
}


/**
 * @brief Load configuration data.
 */
void handle_configure(void)
{
    uint32_t size = 0;

    // Acknowledge the host
    uart_writeb(HOST_UART, 'C');

    // Receive size
    size = (((uint32_t)uart_readb(HOST_UART)) << 24);
    size |= (((uint32_t)uart_readb(HOST_UART)) << 16);
    size |= (((uint32_t)uart_readb(HOST_UART)) << 8);
    size |= ((uint32_t)uart_readb(HOST_UART));

    flash_erase_page(CONFIGURATION_METADATA_PTR);
    flash_write_word(size, CONFIGURATION_SIZE_PTR);

    uart_writeb(HOST_UART, FRAME_OK);
    
    // Retrieve configuration
    load_data(HOST_UART, CONFIGURATION_STORAGE_PTR, size);
}


/**
 * @brief Host interface polling loop to receive configure, update, readback,
 * and boot commands.
 * 
 * @return int
 */
int main(void) {

    uint8_t cmd = 0;

#ifdef EXAMPLE_AES
    // -------------------------------------------------------------------------
    // example encryption using tiny-AES-c
    // -------------------------------------------------------------------------
    struct AES_ctx ctx;
    uint8_t key[16] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 
                        0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf };
    uint8_t plaintext[16] = "0123456789abcdef";

    // initialize context
    AES_init_ctx(&ctx, key);

    // encrypt buffer (encryption happens in place)
    AES_ECB_encrypt(&ctx, plaintext);

    // decrypt buffer (decryption happens in place)
    AES_ECB_decrypt(&ctx, plaintext);
    // -------------------------------------------------------------------------
    // end example
    // -------------------------------------------------------------------------
    br_sha1_context context;
    br_sha1_init(&context);
#endif

    // Initialize IO components
    uart_init();

    // Handle host commands
    while (1) {
        cmd = uart_readb(HOST_UART);

        switch (cmd) {
        case 'C':
            handle_configure();
            break;
        case 'U':
            handle_update();
            break;
        case 'R':
            handle_readback();
            break;
        case 'B':
            handle_boot();
            break;
        default:
            break;
        }
    }
}
