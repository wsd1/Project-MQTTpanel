extern "C"
{
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <err.h>
#include <dirent.h>
#include <time.h>
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
 
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
 
 #include "utils.h"

#define MAC_SIZE	18
#define IP_SIZE		16


// 获取本机mac
int get_local_mac(const char *eth_inf, char *mac){
	struct ifreq ifr;
	int sd;
	
	bzero(&ifr, sizeof(struct ifreq));
	if( (sd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		printf("get %s mac address socket creat error\n", eth_inf);
		return -1;
	}
	
	strncpy(ifr.ifr_name, eth_inf, sizeof(ifr.ifr_name) - 1);
 
	if(ioctl(sd, SIOCGIFHWADDR, &ifr) < 0)
	{
		printf("get %s mac address error\n", eth_inf);
		close(sd);
		return -1;
	}
 
	snprintf(mac, MAC_SIZE, "%02x:%02x:%02x:%02x:%02x:%02x",
		(unsigned char)ifr.ifr_hwaddr.sa_data[0], 
		(unsigned char)ifr.ifr_hwaddr.sa_data[1],
		(unsigned char)ifr.ifr_hwaddr.sa_data[2], 
		(unsigned char)ifr.ifr_hwaddr.sa_data[3],
		(unsigned char)ifr.ifr_hwaddr.sa_data[4],
		(unsigned char)ifr.ifr_hwaddr.sa_data[5]);
 
	close(sd);
	
	return 0;
}
 
// 获取本机ip
int get_local_ip(const char *eth_inf, char *ip){
	int sd;
	struct sockaddr_in sin;
	struct ifreq ifr;
 
	sd = socket(AF_INET, SOCK_DGRAM, 0);
	if (-1 == sd)
	{
		printf("socket error: %s\n", strerror(errno));
		return -1;		
	}
 
	strncpy(ifr.ifr_name, eth_inf, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;
	
	// if error: No such device
	if (ioctl(sd, SIOCGIFADDR, &ifr) < 0)
	{
		printf("ioctl error: %s\n", strerror(errno));
		close(sd);
		return -1;
	}
 
	memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
	snprintf(ip, IP_SIZE, "%s", inet_ntoa(sin.sin_addr));
 
	close(sd);
	return 0;
}

bool convRGBstr(char *str, uint8_t *red, uint8_t *green, uint8_t *blue){
	unsigned int redInt;
	unsigned int greenInt;
	unsigned int blueInt;
	bool OK = false;

	if (sscanf(str, "#%02x%02x%02x", &redInt, &greenInt, &blueInt) == 3)
		OK = true;
	else if (sscanf(str, "%02x%02x%02x", &redInt, &greenInt, &blueInt) == 3)
		OK = true;
	else if (sscanf(str, "0x%02x%02x%02x", &redInt, &greenInt, &blueInt) == 3)
		OK = true;

	if (!OK) return(false);

	(*red) = (uint8_t)(redInt & 0xFF);
	(*green) = (uint8_t)(greenInt & 0xFF);
	(*blue) = (uint8_t)(blueInt & 0xFF);
	return(true);
}

void spinning(){
	char spin[5] = "\\|/-";
	static int spin_idx = 0;
	printf("\r%c", spin[spin_idx++]);
	fflush(stdout);
	spin_idx = spin_idx % 4;
}

tmillis_t GetTimeInMillis(){
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

void SleepMillis(tmillis_t milli_seconds){
	if (milli_seconds <= 0) return;
	struct timespec ts;
	ts.tv_sec = milli_seconds / 1000;
	ts.tv_nsec = (milli_seconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
}


// ################################################################################


//参考之：https://github.com/wsd1/rpi-rgb-led-matrix/blob/master/lib/utf8-internal.h
int utf8_bytes(const char* it) {
  uint8_t cp = *it;
  if( 0 == cp)
	return 0;
  else if (cp < 0x80)
    return 1;
  else if ((cp & 0xE0) == 0xC0)
    return 2;
  else if ((cp & 0xF0) == 0xE0)
    return 3;
  else if ((cp & 0xF8) == 0xF0)
    return 4;
  else if ((cp & 0xFC) == 0xF8)
    return 5;
  else if ((cp & 0xFE) == 0xFC)
    return 6;
  else
	return -1;
}

//拷贝utf8字符串，参数n表示最多字符个数，且如果源超出字数，会截断保证输出字数，且目标尾部会加'\0'
int strncpy_utf8(const char* str, char* dst, int n){
	if (!str || n <= 0) return 0;
	int i = utf8_bytes(str), bytes = 0, chars = 0;

	//循环取字符，直到结尾 或者 字符数超限
	while(i > 0 && chars < n){
		bytes += i; chars++;
		i = utf8_bytes(str + bytes);
	}

	//拷贝到目标 并处理结尾
	if(bytes > 0){
		memcpy(dst, str, bytes);
		dst[bytes] = '\0';
	}

	//返回 字符数量
	return chars;
}

int strlen_utf8(const char* str){
	if (!str) return 0;
	int len = (int)strlen(str), ret = 0;

	for (const char* sptr = str; (sptr - str) < len && *sptr;)	{
		unsigned char ch = (unsigned char)(*sptr);
		if (ch < 0x80)		{
			sptr++;	// ascii
			ret++;
		}
		else if (ch < 0xc0)		{
			sptr++;	// invalid char
		}
		else if (ch < 0xe0)		{
			sptr += 2;
			ret++;
		}
		else if (ch < 0xf0)		{
			sptr += 3;
			ret++;
		}
		else		{
			// 统一4个字节
			sptr += 4;
			ret++;
		}
	}
	return ret;
}

