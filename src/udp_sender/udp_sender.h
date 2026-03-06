#ifndef UDP_SENDER_H
#define UDP_SENDER_H

void udp_sender_task(void *pvParameters);
void udp_socket_close(void);

/* Stage 9: Send a heartbeat JSON packet over the shared UDP socket */
void udp_send_heartbeat(const char *data, int len);

#endif // UDP_SENDER_H
