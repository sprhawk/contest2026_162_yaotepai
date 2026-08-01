/****************************************************************************
 * app/bt_test/bt_test.c
 *
 * Simple BT test: open /dev/ttyHCI0 to trigger BT stack init and LCPU boot.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
  int fd;
  uint8_t hci_reset[] = {0x01, 0x03, 0x0c, 0x00}; /* HCI Reset */
  ssize_t n;
  uint8_t buf[256];

  printf("BT Test: opening /dev/ttyHCI0...\n");

  fd = open("/dev/ttyHCI0", O_RDWR);
  if (fd < 0)
    {
      printf("open failed: %d\n", errno);
      return -1;
    }

  printf("BT Test: sending HCI Reset...\n");

  n = write(fd, hci_reset, sizeof(hci_reset));
  if (n < 0)
    {
      printf("write failed: %d\n", errno);
      close(fd);
      return -1;
    }

  printf("BT Test: sent %zd bytes, waiting for response...\n", n);

  usleep(500000); /* 500ms */

  n = read(fd, buf, sizeof(buf));
  if (n < 0)
    {
      printf("read failed: %d\n", errno);
    }
  else
    {
      printf("BT Test: received %zd bytes:", n);
      for (int i = 0; i < n && i < 32; i++)
        {
          printf(" %02x", buf[i]);
        }
      printf("\n");
    }

  close(fd);
  printf("BT Test: done\n");
  return 0;
}
