#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <libusb-1.0/libusb.h>

#define VID         0x1acc
#define PID         0x0103

#define IFACE       0
#define ALTSETTING  1

#define EP_MIDI_IN  0x83
#define EP_PCM_OUT  0x05

#define BUF_SIZE    512
#define TIMEOUT_MS  1000
#define IFACE_PCM_IN 1

static volatile sig_atomic_t running = 1;
static libusb_device_handle *g_dev = NULL;

static void on_sigint(int sig)
{
    (void)sig;
    running = 0;
}

static double now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return (double)tv.tv_sec * 1000.0 +
           (double)tv.tv_usec / 1000.0;
}

static void dump_hex(const unsigned char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if ((i % 16) == 0)
            printf("%04X: ", i);

        printf("%02X ", buf[i]);

        if ((i % 16) == 15 || i == len - 1)
            printf("\n");
    }
}

static int all_idle(const unsigned char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (buf[i] != 0x00 && buf[i] != 0xFD && buf[i] != 0xFF)
            return 0;
    }

    return 1;
}

static int ctrl_in(libusb_device_handle *dev,
                   uint8_t request,
                   uint16_t value,
                   uint16_t index,
                   unsigned char *buf,
                   uint16_t len)
{
    return libusb_control_transfer(
        dev,
        0xC0,
        request,
        value,
        index,
        buf,
        len,
        2000
    );
}

static int ctrl_out(libusb_device_handle *dev,
                    uint8_t request,
                    uint16_t value,
                    uint16_t index,
                    unsigned char *buf,
                    uint16_t len)
{
    return libusb_control_transfer(
        dev,
        0x40,
        request,
        value,
        index,
        buf,
        len,
        2000
    );
}

static int audio_ctrl_in(libusb_device_handle *dev,
                         uint8_t request,
                         uint16_t value,
                         uint16_t index,
                         unsigned char *buf,
                         uint16_t len)
{
    return libusb_control_transfer(
        dev,
        0xA2,
        request,
        value,
        index,
        buf,
        len,
        2000
    );
}

static int audio_ctrl_out(libusb_device_handle *dev,
                          uint8_t request,
                          uint16_t value,
                          uint16_t index,
                          unsigned char *buf,
                          uint16_t len)
{
    return libusb_control_transfer(
        dev,
        0x22,
        request,
        value,
        index,
        buf,
        len,
        2000
    );
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

    ret = ctrl_in(dev, 0x56, 0, 0, fw, sizeof(fw));
    if (ret < 0)
        return ret;

    printf("firmware raw:");
    for (i = 0; i < ret; i++)
        printf(" %02X", fw[i]);
    printf("\n");

    ret = ctrl_in(dev, 0x49, 0, 0, status, 1);
    if (ret < 0)
        return ret;

    printf("status inicial: 0x%02X\n", status[0]);

    ret = audio_ctrl_in(dev, 0x81, 0x0100, 0, ratebuf, 3);
    if (ret < 0)
        return ret;

    rate = decode_rate(ratebuf);

    printf("rate actual: %u Hz (%02X %02X %02X)\n",
           rate, ratebuf[0], ratebuf[1], ratebuf[2]);

    encode_rate(rate, ratebuf);

    ret = audio_ctrl_out(dev, 0x01, 0x0100, 0x0086, ratebuf, 3);
    if (ret < 0)
        return ret;

    printf("SET_CUR EP86 OK\n");

    ret = audio_ctrl_out(dev, 0x01, 0x0100, 0x0005, ratebuf, 3);
    if (ret < 0)
        return ret;

    printf("SET_CUR EP05 OK\n");

    ret = ctrl_in(dev, 0x49, 0, 0, status, 1);
    if (ret < 0)
        return ret;

    printf("status previo confirm: 0x%02X\n", status[0]);

    wvalue = (uint16_t)(status[0] | 0x20);

    ret = ctrl_out(dev, 0x49, wvalue, 0, NULL, 0);
    if (ret < 0)
        return ret;

    printf("status confirmado: wValue=0x%04X\n", wvalue);
    printf("=== handshake completo ===\n\n");

    return 0;
}

static void *out_thread(void *arg)
{
    unsigned char packet[512];
    unsigned long count = 0;
    int transferred;
    int ret;

    (void)arg;

    memset(packet, 0, sizeof(packet));

    /*
     * Idle framing validado físicamente en playback.
     */
    packet[480] = 0xFD;
    packet[481] = 0xFF;

    while (running) {

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
                        "EP05 OUT error: %s\n",
                        libusb_error_name(ret));

            running = 0;
            break;
        }

        count++;

        if ((count % 10000) == 0) {
            printf("[EP05] %lu packets enviados\n", count);
            fflush(stdout);
        }
    }

    return NULL;
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *dev = NULL;
    pthread_t thread;

    unsigned char buf[BUF_SIZE];

    int transferred;
    int ret;
    int thread_started = 0;

    signal(SIGINT, on_sigint);

    ret = libusb_init(&ctx);
    if (ret < 0) {
        fprintf(stderr,
                "libusb_init: %s\n",
                libusb_error_name(ret));
        return 1;
    }

    dev = libusb_open_device_with_vid_pid(ctx, VID, PID);

    if (!dev) {
        fprintf(stderr,
                "No encuentro AudioLink %04x:%04x\n",
                VID, PID);
        goto out;
    }

    /*
     * El módulo debería estar descargado.
     */
    if (libusb_kernel_driver_active(dev, IFACE) == 1) {
        fprintf(stderr,
                "Hay un kernel driver asociado a iface %d.\n",
                IFACE);
        goto out;
    }

    ret = libusb_claim_interface(dev, IFACE);

    ret = libusb_claim_interface(dev, IFACE_PCM_IN);
    if (ret < 0) {
        fprintf(stderr,
                "claim iface 1: %s\n",
                libusb_error_name(ret));
        goto release;
    }

    ret = libusb_set_interface_alt_setting(
        dev,
        IFACE_PCM_IN,
        ALTSETTING
    );

    if (ret < 0) {
        fprintf(stderr,
                "altsetting iface 1: %s\n",
                libusb_error_name(ret));
        goto release;
    }

    if (ret < 0) {
        fprintf(stderr,
                "claim iface: %s\n",
                libusb_error_name(ret));
        goto out;
    }

    ret = libusb_set_interface_alt_setting(
        dev,
        IFACE,
        ALTSETTING
    );

    if (ret < 0) {
        fprintf(stderr,
                "altsetting: %s\n",
                libusb_error_name(ret));
        goto release;
    }

    ret = do_handshake(dev);

    if (ret < 0) {
        fprintf(stderr,
                "handshake: %s\n",
                libusb_error_name(ret));
        goto release;
    }

    g_dev = dev;

    ret = pthread_create(
        &thread,
        NULL,
        out_thread,
        NULL
    );

    if (ret != 0) {
        fprintf(stderr, "pthread_create failed\n");
        goto release;
    }

    thread_started = 1;

    printf("EP05 idle stream arrancado.\n");
    printf("Escuchando MIDI IN por EP83.\n");
    printf("\n");
    printf("Esperá ~2 segundos, tocá UNA tecla, mantenela y soltala.\n");
    printf("Ctrl-C para terminar.\n\n");

    while (running) {

        transferred = 0;

        ret = libusb_bulk_transfer(
            dev,
            EP_MIDI_IN,
            buf,
            sizeof(buf),
            &transferred,
            TIMEOUT_MS
        );

        if (ret == LIBUSB_ERROR_TIMEOUT)
            continue;

        if (ret < 0) {
            if (running)
                fprintf(stderr,
                        "EP83 IN error: %s\n",
                        libusb_error_name(ret));

            break;
        }

        if (transferred <= 0)
            continue;

        /*
         * Por ahora imprimimos cualquier cosa
         * que no sea únicamente framing idle.
         */
        if (all_idle(buf, transferred))
            continue;

        printf("\n*** EP83 DATA ***\n");
        printf("t=%.3f ms len=%d\n",
               now_ms(),
               transferred);

        dump_hex(buf, transferred);

        printf("\n");
        fflush(stdout);
    }

    running = 0;

    if (thread_started)
        pthread_join(thread, NULL);

release:
    libusb_release_interface(dev, IFACE_PCM_IN);
    libusb_release_interface(dev, IFACE);    

out:
    if (dev)
        libusb_close(dev);

    libusb_exit(ctx);

    return 0;
}