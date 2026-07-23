#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

// REPLACE with your exact CC2650 SensorTag MAC address
#define DEST_MAC "54:6C:0E:B7:20:04"

int main() {
    struct sockaddr_l2 addr = { 0 };
    int sock, status;
    unsigned char buf[64] = { 0 };

    // 1. Create a raw Bluetooth L2CAP socket
    sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) {
        perror("Error: Failed to create Bluetooth socket");
        return 1;
    }

    // 2. Map target configuration parameters (BLE ATT protocol uses Fixed Channel ID 4)
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_cid = htobs(4);
    str2ba(DEST_MAC, &addr.l2_bdaddr);

    printf("Connecting to SensorTag %s via low-level L2CAP channel...\n", DEST_MAC);
    status = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (status < 0) {
        perror("Error: Connection failed. Verify device is broadcasting");
        close(sock);
        return 1;
    }
    printf("Connected successfully!\n");

    // 3. Write raw ATT command packet to enable button notifications
    // Opcode: 0x12 (Write Request) | Handle: 0x004c (CCCD Descriptor) | Value: 0x0001 (Notifications On)
    unsigned char enable_notify[] = { 0x12, 0x4c, 0x00, 0x01, 0x00 };

    printf("Subscribing to button state notification events...\n");
    if (write(sock, enable_notify, sizeof(enable_notify)) < 0) {
        perror("Error: Failed to register notification configuration byte");
        close(sock);
        return 1;
    }

    printf("\nReady! Press or release the side buttons on the SensorTag.\n");
    printf("Press Ctrl+C inside this terminal window to exit.\n\n");

    // 4. Handle incoming notification traffic blocks
    while (1) {
        int len = read(sock, buf, sizeof(buf));
        if (len <= 0) {
            printf("\nConnection lost or closed by the remote device.\n");
            break;
        }

        // Check if packet is a BLE Handle Value Notification (ATT Opcode 0x1b)
        if (buf[0] == 0x1b) {
            // Byte 1-2: Handle attribute source, Byte 3: Button binary bitmask payload
            unsigned char state_byte = buf[3];

            if (state_byte == 0) {
                printf("-> [RELEASED] All buttons let go.\n");
            } else {
                printf("-> [PRESSED] ");
                if (state_byte & 0x01) printf("[LEFT Button] ");
                if (state_byte & 0x02) printf("[RIGHT Button] ");
                if (state_byte & 0x04) printf("[REED Switch] ");
                printf("\n");
            }
        }
    }

    close(sock);
    return 0;
}
