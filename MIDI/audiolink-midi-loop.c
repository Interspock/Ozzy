#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID             0x1acc
#define PID             0x0103

#define IFACE_OUT       0
#define IFACE_IN        1
#define ALTSETTING      1

#define EP_PCM_OUT      0x05
#define EP_MIDI_IN      0x83

#define PACKET_SIZE     512
#define MIDI_SLOT       480
#define SYNC_SLOT       481

static volatile sig_atomic_t running = 1;
static libusb_device_handle *g_dev;

/*
 * Nota C4 ON / OFF.
 *
 * Mandamos status también en el Note Off para que el patrón
 * sea absolutamente inequívoco durante esta prueba.
 */
static const uint8_t test_midi[] = {
    0x90, 0x3c, 0x40,
    0x90, 0x3c, 0x00
};

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

static unsigned int decode_rate(const unsigned char *b)
{
    return (unsigned int)b[0] |
           ((unsigned int)b[1] << 8) |
           ((unsigned int)b[2] << 16);
}

static void encode_rate(unsigned int rate, unsigned char *b)
{
    b[0] = rate & 0xff;
    b[1] = (rate >> 8) & 0xff;
    b[2] = (rate >> 16) & 0xff;
}

static int do_handshake(libusb_device_handle *dev)
{
    unsigned char fw[15] = {0};
    unsigned char status[1] = {0};
    unsigned char ratebuf[3] = {0};
    unsigned int rate;
    uint16_t wvalue;
    int ret;
    int i;

    printf("=== handshake AudioLink ===\n");

    ret = libusb_control_transfer(
        dev, 0xC0, 0x56, 0, 0,
        fw, sizeof(fw), 2000);

    if (ret < 0)
        return ret;

    printf("firmware raw:");
    for (i = 0; i < ret; i++)
        printf(" %02X", fw[i]);
    printf("\n");

    ret = libusb_control_transfer(
        dev, 0xC0, 0x49, 0, 0,
        status, 1, 2000);

    if (ret < 0)
        return ret;

    printf("status inicial: 0x%02X\n", status[0]);

    ret = libusb_control_transfer(
        dev, 0xA2, 0x81, 0x0100, 0,
        ratebuf, 3, 2000);

    if (ret < 0)
        return ret;

    rate = decode_rate(ratebuf);

    printf("rate actual: %u Hz (%02X %02X %02X)\n",
           rate, ratebuf[0], ratebuf[1], ratebuf[2]);

    encode_rate(rate, ratebuf);

    ret = libusb_control_transfer(
        dev, 0x22, 0x01, 0x0100, 0x0086,
        ratebuf, 3, 2000);

    if (ret < 0)
        return ret;

    printf("SET_CUR EP86 OK\n");

    ret = libusb_control_transfer(
        dev, 0x22, 0x01, 0x0100, 0x0005,
        ratebuf, 3, 2000);

    if (ret < 0)
        return ret;

    printf("SET_CUR EP05 OK\n");

    ret = libusb_control_transfer(
        dev, 0xC0, 0x49, 0, 0,
        status, 1, 2000);

    if (ret < 0)
        return ret;

    printf("status previo confirm: 0x%02X\n", status[0]);

    wvalue = status[0] | 0x20;

    ret = libusb_control_transfer(
        dev, 0x40, 0x49, wvalue, 0,
        NULL, 0, 2000);

    if (ret < 0)
        return ret;

    printf("status confirmado: 0x%04X\n", wvalue);
    printf("=== handshake completo ===\n\n");

    return 0;
}

static void *out_thread(void *arg)
{
    unsigned char packet[PACKET_SIZE];
    unsigned long packet_count = 0;
    unsigned int midi_pos = 0;
    unsigned int idle_count = 0;
    int transferred;
    int ret;

    (void)arg;

    while (running) {

        memset(packet, 0, sizeof(packet));

        /*
         * Paquete AudioLink de silencio validado.
         */
        packet[MIDI_SLOT] = 0xFD;
        packet[SYNC_SLOT] = 0xFF;

        /*
         * Cada ~1 segundo iniciamos una ráfaga:
         *
         *   90 3C 40
         *   90 3C 00
         *
         * Un byte MIDI real cada 4 paquetes.
         * Así no excedemos la velocidad física MIDI,
         * incluso si luego probamos a 96 kHz.
         */
        if (idle_count >= 4000) {

            if ((packet_count % 4) == 0) {

                packet[MIDI_SLOT] = test_midi[midi_pos];

                printf("OUT MIDI byte: %02X\n",
                       test_midi[midi_pos]);
                fflush(stdout);

                midi_pos++;

                if (midi_pos >= sizeof(test_midi)) {
                    midi_pos = 0;
                    idle_count = 0;

                    printf("---- secuencia enviada ----\n");
                    fflush(stdout);
                }
            }

        } else {
            idle_count++;
        }

        transferred = 0;

        ret = libusb_bulk_transfer(
            g_dev,
            EP_PCM_OUT,
            packet,
            sizeof(packet),
            &transferred,
            1000
        );

        if (ret < 0) {
            if (running)
                fprintf(stderr,
                        "EP05 OUT: %s\n",
                        libusb_error_name(ret));
            running = 0;
            break;
        }

        packet_count++;
    }

    return NULL;
}

static void dump_midi_in(const unsigned char *buf, int len)
{
    int p;
    int i;

    /*
     * EP83:
     *
     *   [4 MIDI/FD slots] EB
     *
     * Puede haber más de un bloque de 5 bytes en una lectura,
     * así que recorremos todo.
     */
    for (p = 0; p + 4 < len; p += 5) {

        if (buf[p + 4] != 0xEB) {
            printf("IN framing raro: ");
            for (i = 0; i < 5; i++)
                printf("%02X ", buf[p + i]);
            printf("\n");
        }

        for (i = 0; i < 4; i++) {
            if (buf[p + i] != 0xFD)
                printf("IN MIDI byte : %02X\n",
                       buf[p + i]);
        }

        fflush(stdout);
    }
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *dev = NULL;
    pthread_t tx_thread;

    unsigned char inbuf[512];

    int ret;
    int transferred;
    int thread_started = 0;
    int claimed0 = 0;
    int claimed1 = 0;

    signal(SIGINT, sigint_handler);

    ret = libusb_init(&ctx);
    if (ret < 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }

    dev = libusb_open_device_with_vid_pid(ctx, VID, PID);

    if (!dev) {
        fprintf(stderr,
                "No encuentro %04x:%04x\n",
                VID, PID);
        goto out;
    }

    /*
     * Queremos usuariospace completamente dueño del dispositivo.
     */
    if (libusb_kernel_driver_active(dev, IFACE_OUT) == 1) {
        fprintf(stderr,
                "Todavía hay kernel driver en interface 0\n");
        goto out;
    }

    if (libusb_kernel_driver_active(dev, IFACE_IN) == 1) {
        fprintf(stderr,
                "Todavía hay kernel driver en interface 1\n");
        goto out;
    }

    ret = libusb_claim_interface(dev, IFACE_OUT);
    if (ret < 0) {
        fprintf(stderr,
                "claim iface0: %s\n",
                libusb_error_name(ret));
        goto out;
    }

    claimed0 = 1;

    ret = libusb_claim_interface(dev, IFACE_IN);
    if (ret < 0) {
        fprintf(stderr,
                "claim iface1: %s\n",
                libusb_error_name(ret));
        goto out;
    }

    claimed1 = 1;

    ret = libusb_set_interface_alt_setting(
        dev, IFACE_OUT, ALTSETTING);

    if (ret < 0) {
        fprintf(stderr,
                "alt iface0: %s\n",
                libusb_error_name(ret));
        goto out;
    }

    ret = libusb_set_interface_alt_setting(
        dev, IFACE_IN, ALTSETTING);

    if (ret < 0) {
        fprintf(stderr,
                "alt iface1: %s\n",
                libusb_error_name(ret));
        goto out;
    }

    ret = do_handshake(dev);

    if (ret < 0) {
        fprintf(stderr,
                "handshake: %s\n",
                libusb_error_name(ret));
        goto out;
    }

    g_dev = dev;

    ret = pthread_create(
        &tx_thread, NULL, out_thread, NULL);

    if (ret != 0) {
        fprintf(stderr, "pthread_create failed\n");
        goto out;
    }

    thread_started = 1;

    printf("MIDI LOOP TEST\n");
    printf("AudioLink MIDI OUT -> MIDI IN\n");
    printf("MIDI OUT candidate: EP05 offset %d\n",
           MIDI_SLOT);
    printf("\n");
    printf("Esperando retorno por EP83...\n\n");

    while (running) {

        transferred = 0;

        ret = libusb_bulk_transfer(
            dev,
            EP_MIDI_IN,
            inbuf,
            sizeof(inbuf),
            &transferred,
            1000
        );

        if (ret == LIBUSB_ERROR_TIMEOUT)
            continue;

        if (ret < 0) {
            if (running)
                fprintf(stderr,
                        "EP83 IN: %s\n",
                        libusb_error_name(ret));
            break;
        }

        if (transferred > 0)
            dump_midi_in(inbuf, transferred);
    }

    running = 0;

    if (thread_started)
        pthread_join(tx_thread, NULL);

out:

    if (claimed1)
        libusb_release_interface(dev, IFACE_IN);

    if (claimed0)
        libusb_release_interface(dev, IFACE_OUT);

    if (dev)
        libusb_close(dev);

    libusb_exit(ctx);

    return 0;
}

