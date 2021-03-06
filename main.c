/**
 * \file main.c
 * \brief SmartHive LoRa gateway UDP server (Little Endian)
 * \author yusuke <gachapin.2nd@gmail.com>
 */


/** If you use POSIX style non-block socket, enable this */
#define ENABLE_POSIX_NONBLOCK

/** Enable debug print */
//#define ENABLE_DEBUG

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#if defined(ENABLE_POSIX_NONBLOCK)
#   include <fcntl.h>
#else /* defined(ENABLE_POSIX_NONBLOCK) */
#   include <sys/ioctl.h>
#endif /* defined(ENABLE_POSIX_NONBLOCK) */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>



#define LORA_HEADER_SIZE  (3)
#define LORA_PAYLOAD_SIZE (40)
#define LORA_PACKET_SIZE  (LORA_HEADER_SIZE + LORA_PAYLOAD_SIZE)
#define MAX_LORA_CLIENTS  (100)

#define UDP_PROTOCOL_VERSION  (0x12)
#define UDP_PKTID_PUSH_DATA   (0)
#define UDP_HEADER_SIZE       (4)
#define UDP_PACKET_SIZE       (UDP_HEADER_SIZE + LORA_PACKET_SIZE)
enum tag_UDP_CLIENT_IDS {
    UDP_CLIENT_ID_DUMMY = 0,
    UDP_CLIENT_ID_MAIN,
    MAX_UDP_CLIENT_IDS
};

#define UDP_SERVER_PORT (50812)
#define UDP_SERVER_ADDR ("127.0.0.1")
#define UDP_SERVER_TIMEOUT_SEC  (3)
#define UDP_SERVER_TIMEOUT_USEC (0)

#define UDP_BUFSIZE (256)
#define CSV_BUFSIZE (512)



#if !defined(MAX_)
#   define MAX_(a,b) (a<b?b:a)
#endif /* !defined(MAX_) */



struct delegate_plugin {
    /** [opt] Initalizing handler */
    bool (*p_init_fn)(
            unsigned udp_id_,               /**< [in] source UDP client ID */
            struct delegate_plugin *p_      /**< [in,out] delegate plugin info */
            );
    /** [opt] De-initalizing hander */
    void (*p_deinit_fn)(
            unsigned udp_id_,               /**< [in] source UDP client ID */
            struct delegate_plugin *p_      /**< [in,out] delegate plugin info */
            );
    /** [must] Generating CSV hander */
    bool (*p_generate_csv_fn)(
            struct delegate_plugin *p_,     /**< [in,out] delegate plugin info */
            const uint8_t *p_lora_,         /**< [in] LoRa packet data */
            size_t bufsize_,                /**< [in] size of p_buf_ area in byte */
            char *p_buf_                    /**< [in,out] string buffer for output CSV */
            );
    /** [must] Send CSV handler toward to forwarding-server */
    bool (*p_send_to_server_fn)(
            struct delegate_plugin *p_,     /**< [in,out] delegate plugin info */
            const char *p_csv_              /**< [in] CSV string to send */
            );
    /** [opt] User data */
    void *p_user;
};
static struct delegate_plugin g_delegate_[MAX_UDP_CLIENT_IDS];



static uint8_t g_lora_histories_[MAX_UDP_CLIENT_IDS][MAX_LORA_CLIENTS][LORA_PACKET_SIZE];



static volatile sig_atomic_t g_do_term_ = 0;



static void sig_handler(int sig)
{
    g_do_term_ = 1;
    return;
}



static double le16_to_double_(const uint8_t *p_)
{
    uint16_t v = *(const uint16_t *)p_;
    return (double)v;
}



static double be16_to_double_(const uint8_t *p_)
{
    uint16_t v = 0;

    assert(p_);

    v |= *p_++;
    v <<= 8;
    v |= *p_;

    return (double)v;
}



static double le32_to_double_(const uint8_t *p_)
{
    uint32_t v = *(const uint32_t *)p_;
    return (double)v;
}



static double be32_to_double_(const uint8_t *p_)
{
    uint32_t v = 0;

    assert(p_);

    v |= *p_++;
    v <<= 8;
    v |= *p_++;
    v <<= 8;
    v |= *p_++;
    v <<= 8;
    v |= *p_;

    return (double)v;
}



#define UDP_MIKE_SERVER_PORT (50910)
#define UDP_MIKE_SERVER_ADDR ("127.0.0.1")

/** User data for delegation plugin of Mike's Spreadsheet forwarder */
struct mike_info {
    struct sockaddr_in sa;  /**< Forwarding server's socket address */
    int socket_fd;          /**< Forwarding server's socket FD */
    unsigned gw_id;         /**< LoRa GW UDP client ID */
};
static struct mike_info g_mike_info[MAX_UDP_CLIENT_IDS] = { 0 };



static bool init_mike_(unsigned udp_id_, struct delegate_plugin *p_)
{
    struct mike_info *p_info = &g_mike_info[udp_id_];
    int fd = -1;

    assert(udp_id_ < MAX_UDP_CLIENT_IDS);
    assert(p_);

    assert(UDP_CLIENT_ID_MAIN == udp_id_);
    assert(p_info->gw_id == 0);

    if (1 != inet_pton(AF_INET,
                UDP_MIKE_SERVER_ADDR, &p_info->sa.sin_addr.s_addr)) {
        perror("inet_pton() for Mike");
        return false;

    }
    p_info->sa.sin_family = AF_INET;
    p_info->sa.sin_port   = htons(UDP_MIKE_SERVER_PORT);

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        perror("client socket for Mike");
        return false;
    }
    p_info->socket_fd = fd;
    p_info->gw_id     = udp_id_;

    p_->p_user = p_info;

    return true;
}



static void deinit_mike_(unsigned udp_id_, struct delegate_plugin *p_)
{
    struct mike_info *p_info = &g_mike_info[udp_id_];

    assert(p_);
    assert(UDP_CLIENT_ID_MAIN == udp_id_);

    if (0 <= p_info->socket_fd) {
        close(p_info->socket_fd);
        memset(p_info, 0, sizeof(*p_info));
    }

    p_->p_user = NULL;

    return;
}



static bool generate_csv_mike_(struct delegate_plugin *p_,
        const uint8_t *p_lora_, size_t bufsize_, char *p_buf_)
{
    struct mike_info *p_info = (struct mike_info *)p_->p_user;
    double lat_n = 0.0;
    double lon_e = 0.0;
    double temp[4] = { 0.0 };
    double rh[4] = { 0.0 };
    double vol[4] = { 0.0 };
    double weight = 0.0;
    int ret = 0;

    assert(p_);
    assert(p_lora_);
    assert(bufsize_);
    assert(p_buf_);
    assert(p_info);

    /*
     * Mike's server format CSV
     *
     *  CSV:
     *      "Method,WorkSheetName,(...Lora CSV string)"
     *
     *      * Method
     *          * "write" : write Lora CSV data to WorkSheetName
     *          * "close" : close Mike's server socket
     *      * WorkSheetName
     *          Worksheet name in the Google Spreadsheet.
     *          If you specify a name that does not exist in the
     *          spreadsheet, Google Spreadsheet will automatically
     *          generate it.
     */

    lat_n   = le32_to_double_(&p_lora_[9]);
    lon_e   = le32_to_double_(&p_lora_[13]);
    temp[0] = le16_to_double_(&p_lora_[17]);
    temp[1] = le16_to_double_(&p_lora_[19]);
    temp[2] = le16_to_double_(&p_lora_[21]);
    temp[3] = le16_to_double_(&p_lora_[23]);
    rh[0]   = le16_to_double_(&p_lora_[25]);
    rh[1]   = le16_to_double_(&p_lora_[27]);
    rh[2]   = le16_to_double_(&p_lora_[29]);
    rh[3]   = le16_to_double_(&p_lora_[31]);
    vol[0]  = le16_to_double_(&p_lora_[33]);
    vol[1]  = le16_to_double_(&p_lora_[35]);
    vol[2]  = le16_to_double_(&p_lora_[37]);
    vol[3]  = le16_to_double_(&p_lora_[39]);
    weight  = le16_to_double_(&p_lora_[41]);

    ret = snprintf(p_buf_, bufsize_,
            "write"                         /* Method */
            ",%02u-%02u"                    /* WorkSheetName (gw_id-lora_id) */
            ",%02u%02u%02u%02u%02u%02u"     /* yymmddHHMMSS */
            ",%lf"                          /* Lon */
            ",%lf"                          /* Lat */
            ",%lf,%lf,%lf"                  /* Tem1,Hum1,Vol1 */
            ",%lf,%lf,%lf"                  /* Tem2,Hum2,Vol2 */
            ",%lf,%lf,%lf"                  /* Tem3,Hum3,Vol3 */
            ",%lf,%lf,%lf"                  /* Tem4,Hum4,Vol4 */
            ",%lf"                          /* Weight */
            , p_info->gw_id, p_lora_[0]
            , p_lora_[3], p_lora_[4], p_lora_[5]
            , p_lora_[6], p_lora_[7], p_lora_[8]
            , lon_e   / 1000000
            , lat_n   / 1000000
            , temp[0] / 10
            , rh[0]   / 10
            , vol[0]  / 10
            , temp[1] / 10
            , rh[1]   / 10
            , vol[1]  / 10
            , temp[2] / 10
            , rh[2]   / 10
            , vol[2]  / 10
            , temp[3] / 10
            , rh[3]   / 10
            , vol[3]  / 10
            , weight  / 100
            );
    if (ret < 0 || bufsize_ <= ret) {
        fprintf(stderr, "snprintf() [%d]: truncated\n", __LINE__);
        return false;
    }
    p_buf_[bufsize_ - 1] = '\0';

    return true;
}



static bool send_to_server_mike_(struct delegate_plugin *p_, const char *p_csv_)
{
    struct mike_info *p_info = (struct mike_info *)p_->p_user;
    size_t len = 0;
    ssize_t nr = -1;

    assert(p_);
    assert(p_csv_);
    assert(p_info);

    len = 1 + strlen(p_csv_);
    nr = sendto(p_info->socket_fd, p_csv_, len, 0 ,
            (struct sockaddr *)&p_info->sa, sizeof(p_info->sa));
    if (nr != len) {
        perror("sendto() for Mike");
        return false;
    }

    return true;
}



static bool delegate_(size_t len_, const uint8_t *p_udp_)
{
    static char csv[CSV_BUFSIZE];

    uint8_t *p_hist = NULL;
    const uint8_t *p_lora = NULL;

    uint8_t udp_id = 0;
    uint8_t lora_id = 0;

    assert(len_);
    assert(p_udp_);

    /*
     * UDP packet format:
     *
     *  |<----------- B bytes ----------->|
     *  |               |<-- B-4 bytes -->|
     *  +---+---+---+---+-----.......-----+
     *  | A | B | C | D |        E        |
     *  +---+---+---+---+-----.......-----+
     *
     *      * A (1 byte) : Protocol version
     *      * B (1 byte) : Length (from A to E)
     *      * C (1 byte) : UDP client ID
     *      * D (1 byte) : Packet type
     *      * E (B-4 bytes) : LoRa data (max 127 bytes)
     */
    if (len_ != UDP_PACKET_SIZE) {
        fprintf(stderr, "Invalid UDP packet size: %lu\n", len_);
        return false;
    }
    if (p_udp_[0] != UDP_PROTOCOL_VERSION ||
            p_udp_[1] != UDP_PACKET_SIZE ||
            p_udp_[3] != UDP_PKTID_PUSH_DATA) {
        fprintf(stderr, "Invalid UDP packet format\n");
        return false;
    }

    udp_id = p_udp_[2];
    if (MAX_UDP_CLIENT_IDS <= udp_id) {
        fprintf(stderr, "Invalid UDP client ID: %u\n", udp_id);
        return false;
    }

    /*
     * LoRa data format:
     *
     *  All multi-byte fields are big endian.
     *
     *  |<------------------------ 43 bytes -------------------------->|
     *  |<- 3 bytes ->|<---------------- 40 bytes -------------------->|
     *  +----+---+----+------+------+-----+--------+------+-------+----+
     *  | ID | N | PI | DATE | TIME | GPS | TEMPx4 | RHx4 | VOLx4 | WT |
     *  +----+---+----+------+------+-----+--------+------+-------+----+
     *
     *      * Header (3 bytes)
     *          * ID (1 byte) : Device ID
     *          * N (1 byte) : Packet serial number
     *          * PI (1 byte) : Reserved (always 1)
     *      * Payload (40 bytes)
     *          * DATE (3 bytes) year, month, day
     *          * TIME (3 bytes) hour, minute, second
     *          * GPS (8 bytes) lat-N (4 bytes), lon-E (4 bytes)
     *          * TEMPx4 (8 bytes) temperature (each 2 bytes) x 4
     *          * RHx4 (8 bytes) RH. (each 2 bytes) x 4
     *          * VOLx4 (8 bytes) volume (each 2 bytes) x 4
     *          * WT (2 bytes) weight
     */
    p_lora = &p_udp_[4];
    lora_id = p_lora[0];
    if (MAX_LORA_CLIENTS <= lora_id) {
        fprintf(stderr, "Invalid LoRa client ID: %u\n", lora_id);
        return false;
    }

    p_hist = g_lora_histories_[udp_id][lora_id];
    if (!memcmp(p_hist, p_lora, LORA_PACKET_SIZE)) {
#if defined(ENABLE_DEBUG)
        fprintf(stderr, "same data exists\n");
#endif /* defined(ENABLE_DEBUG) */
        return true;
    }

    /* new data arrival */
#if defined(ENABLE_DEBUG)
    fprintf(stderr, "new data arrival\n");
#endif /* defined(ENABLE_DEBUG) */
    memcpy(p_hist, p_lora, LORA_PACKET_SIZE);

    assert(g_delegate_[udp_id].p_generate_csv_fn);
    if (!g_delegate_[udp_id].p_generate_csv_fn(&g_delegate_[udp_id],
                p_lora, sizeof(csv), csv)) {
        fprintf(stderr, "Generate CSV failed\n");
        return false;
    }

    assert(g_delegate_[udp_id].p_send_to_server_fn);
    if (!g_delegate_[udp_id].p_send_to_server_fn(&g_delegate_[udp_id], csv)) {
        fprintf(stderr, "Send CSV failed\n");
        return false;
    }

    return true;
}



static void cleanup_delegate_(void)
{
    int i = 0;

    for (i = 0; i < MAX_UDP_CLIENT_IDS; ++i) {
        if (g_delegate_[i].p_deinit_fn) {
            g_delegate_[i].p_deinit_fn(i, &g_delegate_[i]);
        }
    }

    return;
}



static bool setup_delegate_(void)
{
    unsigned i = 0;

    memset(g_delegate_, 0, sizeof(g_delegate_));

    for (i = 0; i < MAX_UDP_CLIENT_IDS; ++i) {
        switch (i) {
        case UDP_CLIENT_ID_DUMMY:
#if defined(ENABLE_DEBUG)
            fprintf(stderr, "dummy device id %u, skipped\n", i);
#endif /* defined(ENABLE_DEBUG) */
            break;

        case UDP_CLIENT_ID_MAIN:
            g_delegate_[i].p_init_fn           = init_mike_;
            g_delegate_[i].p_deinit_fn         = deinit_mike_;
            g_delegate_[i].p_generate_csv_fn   = generate_csv_mike_;
            g_delegate_[i].p_send_to_server_fn = send_to_server_mike_;
            if (!g_delegate_[i].p_init_fn(i, &g_delegate_[i])) {
                cleanup_delegate_();
                return false;
            }
            break;

        default:
#if defined(ENABLE_DEBUG)
            fprintf(stderr, "unknown device id %u, skipped\n", i);
#endif /* defined(ENABLE_DEBUG) */
            break;
        }
    }

    return true;
}



int main(void)
{
    static uint8_t buf[UDP_BUFSIZE];

    int socket_fd = -1;

    int nfds = -1;
    fd_set rfds_init;
    fd_set rfds;
    struct timeval timeout_init;
    struct timeval tv;

    int ret = -1;



#if defined(ENABLE_DEBUG)
    fprintf(stderr, "BEGIN\n");
#endif /* defined(ENABLE_DEBUG) */

    g_do_term_ = 0;
    memset(g_lora_histories_, 0, sizeof(g_lora_histories_));
    if (!setup_delegate_()) {
        fprintf(stderr, "Fatal error: setup_delegate_()\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, sig_handler);

    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        perror("server socket");
        return EXIT_FAILURE;
    }

    /* we don't want to use recvfrom(), so treat bind() here */
    {
        struct sockaddr_in sa;

        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(UDP_SERVER_PORT);
        /* sa.sin_addr.s_addr = INADDR_ANY; */
        if (1 != inet_pton(AF_INET, UDP_SERVER_ADDR, &sa.sin_addr.s_addr) ||
                bind(socket_fd, (struct sockaddr *)&sa, sizeof(sa))) {
            perror("bind");
            close(socket_fd), socket_fd = -1;
            return EXIT_FAILURE;
        }
    }

#if defined(ENABLE_POSIX_NONBLOCK)
    {   /* POSIX style */
        int flags = fcntl(socket_fd, F_GETFL, 0);

        flags |= O_NONBLOCK;
        ret = fcntl(socket_fd, F_SETFL, flags);
        if (ret) {
            perror("fcntl(O_NONBLOCK)");
            close(socket_fd), socket_fd = -1;
            return EXIT_FAILURE;
        }
    }
#else /* defined(ENABLE_POSIX_NONBLOCK) */
    {   /* old style */
        int blk = 1;

        ret = ioctl(socket_fd, FIONBIO, &blk);
        if (ret) {
            perror("ioctl(FIONBIO)");
            close(socket_fd), socket_fd = -1;
            return EXIT_FAILURE;
        }
    }
#endif /* defined(ENABLE_POSIX_NONBLOCK) */

    nfds = -1;
    FD_ZERO(&rfds_init);
    FD_SET(socket_fd, &rfds_init);
    nfds = MAX_(nfds, socket_fd);
    ++nfds;

    timeout_init.tv_sec  = UDP_SERVER_TIMEOUT_SEC;
    timeout_init.tv_usec = UDP_SERVER_TIMEOUT_USEC;

    for ( ; !g_do_term_; ) {

        memcpy(&rfds, &rfds_init, sizeof(rfds));
        tv = timeout_init;
        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select");

        } else if (0 == ret) {
#if defined(ENABLE_DEBUG)
            fprintf(stderr, "select() timeout\n");
#endif /* defined(ENABLE_DEBUG) */

        } else if (FD_ISSET(socket_fd, &rfds)) {
            /*
             * UDP 通信ではオプション無しで recv() や recvfrom() を使うと
             * 受信キューからその回のデータを消してしまう。従って、受信
             * バッファのサイズが受信データよりも小さい場合、受信しきら
             * なかったデータは消えてしまう。 MSG_PEEK オプションを指定
             * すればこの挙動を抑制可能である (つまり次も同じデータを受信
             * できることになる) 。
             *
             * 応用として可変長データの受信が可能になる。つまり
             * クライアントで送信データの先頭付近にデータ長を含めておき、
             * サーバーでは MSG_PEEK で一度空読みしそれを取得、適切な
             * バッファサイズを確保した上で再度 (MSG_PEEK 無しで) 受信
             * 処理をすればいい。
             */
            ssize_t nr = recv(socket_fd, buf, sizeof(buf) - 1, 0);
            if (nr < 0) {
                if (EAGAIN == errno) {
                    fprintf(stderr, "recv: data isn't yet reached\n");
                } else {
                    perror("recv");
                    g_do_term_ = 1;
                }

            } else if (0 == nr) {
                fprintf(stderr,
                        "recv: peer shutted down or "
                        "0 byte packet received\n");

            } else {
#if defined(ENABLE_DEBUG)
                fprintf(stderr, "call delegate_()\n");
#endif /* defined(ENABLE_DEBUG) */
                delegate_(nr, buf);
            }
        } else {
            fprintf(stderr,
                    "select() didn't timeout but no sockets signalled\n");
        }
    }

    close(socket_fd), socket_fd = -1;

    cleanup_delegate_();

#if defined(ENABLE_DEBUG)
    fprintf(stderr, "END\n");
#endif /* defined(ENABLE_DEBUG) */

    return EXIT_SUCCESS;
}



/* vim: set ts=4 sts=4 sw=4 expandtab autoindent : */
