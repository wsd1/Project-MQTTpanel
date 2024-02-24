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

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "led-matrix.h"
#include "graphics.h"

#include "json.h"
#include "json_util.h"
#include "mosquitto.h"

#include "queue.h"
#include "tween.h"

#define mqtt_host "192.168.31.79"
#define mqtt_port 1883
#define RECONNECT_TIMEOUT 10000
#define MSG_LENGTH_MAX 128

int sflags = 0;
#define json_object_to_json_string(obj) json_object_to_json_string_ext(obj,sflags)


//using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;





void DisplayAnimation(RGBMatrix *matrix, FrameCanvas *off_canvas, int vsync_multiple);


// ################################################################################

typedef struct
{
	char root[64];
	char clientid[64];
	char host[64];
	uint16_t port;
} mqttCfg_t;

typedef int64_t tmillis_t;




typedef struct {
    Tween* tween;
	char txt[MSG_LENGTH_MAX*3];	//根据最长的msg设定buf utf8 汉字是3字节
}msg_t;

QUEUE_DECLARATION(chooQ, msg_t, 100);	//msg_t x 100 
QUEUE_DEFINITION(chooQ, msg_t);
struct chooQ chooQueue;


Tween_Engine* glbEngine = NULL;



// ################################################################################

json_object *ConfigJSON;

struct mosquitto *mosq;
RGBMatrix::Options panelOptions;

RGBMatrix *glbCanvas;
FrameCanvas *offscreen_canvas;
rgb_matrix::Font glbFont;

mqttCfg_t glbMqttCfg;
volatile bool Interrupt = false;

tmillis_t glbLastConnection = 0;
bool glbIsOnline = false;

// ################################################################################

static void spinning(){
	char spin[5] = "\\|/-";
	static int spin_idx = 0;
	printf("\r%c", spin[spin_idx++]);
	fflush(stdout);
	spin_idx = spin_idx % 4;
}

static tmillis_t GetTimeInMillis()
{
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static void SleepMillis(tmillis_t milli_seconds)
{
	if (milli_seconds <= 0) return;
	struct timespec ts;
	ts.tv_sec = milli_seconds / 1000;
	ts.tv_nsec = (milli_seconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
}

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


// ################################################################################





static void setOffline(){
	glbIsOnline = false;
}

static bool isOnline(){
	return glbIsOnline;
}
static bool isTimeoutForConnect(){
	return (GetTimeInMillis() - glbLastConnection >= RECONNECT_TIMEOUT);
}

static void setTimeoutConnected(){
	glbIsOnline = true;
	glbLastConnection = GetTimeInMillis();
}

static void setTimeoutConnecting(){
	glbLastConnection = GetTimeInMillis();
}

static void InterruptHandler(int signo){
	Interrupt = true;
}

static int usage(const char *progname){
	fprintf(stderr, "usage: %s [options] <image> [option] [<image> ...]\n", progname);

	fprintf(stderr, "Options:\n"
		"\t-O<streamfile>            : Output to stream-file instead of matrix (Don't need to be root).\n"
		"\t-C                        : Center images.\n"

		"\nThese options affect images following them on the command line:\n"
		"\t-w<seconds>               : Regular image: "
		"Wait time in seconds before next image is shown (default: 1.5).\n"
		"\t-t<seconds>               : "
		"For animations: stop after this time.\n"
		"\t-l<loop-count>            : "
		"For animations: number of loops through a full cycle.\n"
		"\t-D<animation-delay-ms>    : "
		"For animations: override the delay between frames given in the\n"
		"\t                            gif/stream animation with this value. Use -1 to use default value.\n"

		"\nOptions affecting display of multiple images:\n"
		"\t-f                        : "
		"Forever cycle through the list of files on the command line.\n"
		"\t-s                        : If multiple images are given: shuffle.\n"
		"\nDisplay Options:\n"
		"\t-V<vsync-multiple>        : Expert: Only do frame vsync-swaps on multiples of refresh (default: 1)\n"
		"\t-L                        : Large display, in which each chain is 'folded down'\n"
		"\t                            in the middle in an U-arrangement to get more vertical space.\n"
		"\t-R<angle>                 : Rotate output; steps of 90 degrees\n"
		);

	fprintf(stderr, "\nGeneral LED matrix options:\n");
	rgb_matrix::PrintMatrixFlags(stderr);

	fprintf(stderr,
		"\nSwitch time between files: "
		"-w for static images; -t/-l for animations\n"
		"Animated gifs: If both -l and -t are given, "
		"whatever finishes first determines duration.\n");

	fprintf(stderr, "\nThe -w, -t and -l options apply to the following images "
		"until a new instance of one of these options is seen.\n"
		"So you can choose different durations for different images.\n");

	return(1);
}

// ################################################################################


// 在 ConfigJSON.panel对象中检查Key，不存在则用 panelOptions[key] 值创建；
// 如果 JSONpref == true，那么 panelOptions[key] 设置为json中的值；
void setPanelConfig(char *Key, bool Value, bool JSONpref){
	struct json_object *rootObject;
	if (!json_object_object_get_ex(ConfigJSON, "panel", &rootObject))
		return;



	//上面在全局结构变量 panelOptions 中找到的相应key 和 value值
	bool *ptrKey;
	if (strcmp(Key, "show_refresh_rate") == 0)
		ptrKey = &panelOptions.show_refresh_rate;

	else if (strcmp(Key, "inverse_colors") == 0)
		ptrKey = &panelOptions.inverse_colors;

	else
		return;


	//json结构panel对象下面，如果没有就创建（值继承panelOptions.key）
	struct json_object *keyObject;
	if (!json_object_object_get_ex(rootObject, Key, &keyObject))	
	{
		keyObject = json_object_new_boolean((json_bool)(*ptrKey));
		json_object_object_add(rootObject, Key, keyObject);
	}


	//函数参数JSONpref 若为true，则 panelOptions.key 获取json对象中相应key值
	if (JSONpref == true)
		(*ptrKey) = (bool *) json_object_get_boolean(keyObject);
	else
		(*ptrKey) = Value;
}


void setPanelConfig(char *Key, char *Value, bool JSONpref){
	struct json_object *rootObject;
	if (!json_object_object_get_ex(ConfigJSON, "panel", &rootObject))
		return;


	char *ptrKey;
	if (strcmp(Key, "hardware_mapping") == 0)
		ptrKey = (char *)panelOptions.hardware_mapping;

	else if (strcmp(Key, "led_rgb_sequence") == 0)
		ptrKey = (char *)panelOptions.led_rgb_sequence;

	else
		return;


	struct json_object *keyObject;
	if (!json_object_object_get_ex(rootObject, Key, &keyObject))
	{
		keyObject = json_object_new_string(ptrKey);
		json_object_object_add(rootObject, Key, keyObject);
	}
	if (JSONpref == true)
		strcpy((char *)json_object_get_string(keyObject), ptrKey);
	else
		strcpy(Value, ptrKey);
}


void setPanelConfig(char *Key, int Value, bool JSONpref){
	struct json_object *rootObject;
	if (!json_object_object_get_ex(ConfigJSON, "panel", &rootObject))
		return;


	int *ptrKey;
	if (strcmp(Key, "rows") == 0)
		ptrKey = &panelOptions.rows;

	else if (strcmp(Key, "cols") == 0)
		ptrKey = &panelOptions.cols;

	else if (strcmp(Key, "chain_length") == 0)
		ptrKey = &panelOptions.chain_length;

	else if (strcmp(Key, "parallel") == 0)
		ptrKey = &panelOptions.parallel;

	else if (strcmp(Key, "multiplexing") == 0)
		ptrKey = &panelOptions.multiplexing;

	else if (strcmp(Key, "brightness") == 0)
		ptrKey = &panelOptions.brightness;

	else if (strcmp(Key, "scan_mode") == 0)
		ptrKey = &panelOptions.scan_mode;

	else if (strcmp(Key, "pwm_bits") == 0)
		ptrKey = &panelOptions.pwm_bits;

	else if (strcmp(Key, "pwm_lsb_nanoseconds") == 0)
		ptrKey = &panelOptions.pwm_lsb_nanoseconds;

	else if (strcmp(Key, "row_address_type") == 0)
		ptrKey = &panelOptions.row_address_type;
	else
		return;


	struct json_object *keyObject;
	if (!json_object_object_get_ex(rootObject, Key, &keyObject))
	{
		keyObject = json_object_new_int((int32_t)(*ptrKey));
		json_object_object_add(rootObject, Key, keyObject);
	}
	if (JSONpref == true)
		(*ptrKey) = json_object_get_int(keyObject);
	else
		(*ptrKey) = Value;
}




// ConfigJSON.panel[key] => panelOptions[key] 
// JSON => struct
void setPanelConfig(char *Key){	
	if (strcmp(Key, "show_refresh_rate") == 0)
		setPanelConfig(Key, true, true);

	else if (strcmp(Key, "inverse_colors") == 0)
		setPanelConfig(Key, true, true);

	else if (strcmp(Key, "hardware_mapping") == 0)
		setPanelConfig(Key, (char *)NULL, true);

	else if (strcmp(Key, "led_rgb_sequence") == 0)
		setPanelConfig(Key, (char *)NULL, true);

	else if (strcmp(Key, "rows") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "cols") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "chain_length") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "parallel") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "multiplexing") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "brightness") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "scan_mode") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "pwm_bits") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "pwm_lsb_nanoseconds") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "row_address_type") == 0)
		setPanelConfig(Key, (int)0, true);

}


// ################################################################################
int initPanel(int argc, char *argv[]){

	// ConfigJSON.panel[key] => panelOptions[key] 
	setPanelConfig((char *) "hardware_mapping");
	setPanelConfig((char *) "led_rgb_sequence");
	setPanelConfig((char *) "rows");
	setPanelConfig((char *) "cols");
	setPanelConfig((char *) "chain_length");
	setPanelConfig((char *) "parallel");
	setPanelConfig((char *) "multiplexing");
	setPanelConfig((char *) "brightness");
	setPanelConfig((char *) "scan_mode");
	setPanelConfig((char *) "pwm_bits");
	setPanelConfig((char *) "pwm_lsb_nanoseconds");
	setPanelConfig((char *) "row_address_type");
	setPanelConfig((char *) "show_refresh_rate");
	setPanelConfig((char *) "inverse_colors");

	struct json_object *rootObject;

	//从JSON中读取其他配置 ConfigJSON.mqtt[] => glbMqttCfg[] 
	if (json_object_object_get_ex(ConfigJSON, "mqtt", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "clientid", &keyObject))
		{
			keyObject = json_object_new_string("defaultID:chooPanel");
			json_object_object_add(rootObject, "clientid", keyObject);
		}
		strcpy(glbMqttCfg.clientid, json_object_get_string(keyObject));

		if (!json_object_object_get_ex(rootObject, "host", &keyObject))
		{
			keyObject = json_object_new_string("mqtt.eclipseprojects.io");	//paho.mqtt.c demo broker
			json_object_object_add(rootObject, "host", keyObject);
		}
		strcpy(glbMqttCfg.host, json_object_get_string(keyObject));

		if (!json_object_object_get_ex(rootObject, "port", &keyObject))
		{
			keyObject = json_object_new_int(1883);
			json_object_object_add(rootObject, "port", keyObject);
		}
		glbMqttCfg.port = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "root", &keyObject))
		{
			keyObject = json_object_new_string("/chooPanel");
			json_object_object_add(rootObject, "root", keyObject);
		}
		strcpy(glbMqttCfg.root, json_object_get_string(keyObject));

	}
	// printf("JSON(date):%s\n", json_object_get_string(rootObject));



	/*
	//从JSON中读取其他配置 ConfigJSON.background[] => displayImage[] 
	if (json_object_object_get_ex(ConfigJSON, "background", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "image", &keyObject))
		{
			keyObject = json_object_new_string("Alien.gif");
			json_object_object_add(rootObject, "image", keyObject);
		}
		displayImage.image = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("images/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayImage.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(1);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayImage.show = json_object_get_boolean(keyObject);
	}
	// printf("JSON(background):%s\n", json_object_get_string(rootObject));

	//从JSON中读取其他配置 ConfigJSON.time[] => displayTime[] 
	if (json_object_object_get_ex(ConfigJSON, "time", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "format", &keyObject))
		{
			keyObject = json_object_new_string("%H:%M");
			json_object_object_add(rootObject, "format", keyObject);
		}
		displayTime.format = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("/usr/local/share/fonts/rgbmatrix/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayTime.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "font", &keyObject))
		{
			keyObject = json_object_new_string("8x13.bdf");
			json_object_object_add(rootObject, "font", keyObject);
		}
		displayTime.fontname = json_object_get_string(keyObject);
		fetchFont(&displayTime);

		if (!json_object_object_get_ex(rootObject, "x", &keyObject))
		{
			keyObject = json_object_new_int(8);
			json_object_object_add(rootObject, "x", keyObject);
		}
		displayTime.x = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "y", &keyObject))
		{
			keyObject = json_object_new_int(10);
			json_object_object_add(rootObject, "y", keyObject);
		}
		displayTime.y = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(1);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayTime.show = json_object_get_boolean(keyObject);

		if (!json_object_object_get_ex(rootObject, "color", &keyObject))
		{
			keyObject = json_object_new_string("FF00FF");
			json_object_object_add(rootObject, "color", keyObject);
		}
		convRGBstr((char *)json_object_get_string(keyObject), &displayTime.red, &displayTime.green, &displayTime.blue);

		if (!json_object_object_get_ex(rootObject, "timezone", &keyObject))
		{
			keyObject = json_object_new_string(getenv("TZ"));
			json_object_object_add(rootObject, "timezone", keyObject);
		}
		displayTime.timezone = json_object_get_string(keyObject);
	}
	// printf("JSON(time):%s\n", json_object_get_string(rootObject));

	//从JSON中读取其他配置 ConfigJSON.date[] => displayDate[] 
	if (json_object_object_get_ex(ConfigJSON, "date", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "format", &keyObject))
		{
			keyObject = json_object_new_string("%Y/%m/%d");
			json_object_object_add(rootObject, "format", keyObject);
		}
		displayDate.format = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("/usr/local/share/fonts/rgbmatrix/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayDate.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "font", &keyObject))
		{
			keyObject = json_object_new_string("6x9.bdf");
			json_object_object_add(rootObject, "font", keyObject);
		}
		displayDate.fontname = json_object_get_string(keyObject);
		fetchFont(&displayDate);

		if (!json_object_object_get_ex(rootObject, "x", &keyObject))
		{
			keyObject = json_object_new_int(8);
			json_object_object_add(rootObject, "x", keyObject);
		}
		displayDate.x = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "y", &keyObject))
		{
			keyObject = json_object_new_int(30);
			json_object_object_add(rootObject, "y", keyObject);
		}
		displayDate.y = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(1);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayDate.show = json_object_get_boolean(keyObject);

		if (!json_object_object_get_ex(rootObject, "color", &keyObject))
		{
			keyObject = json_object_new_string("FF00FF");
			json_object_object_add(rootObject, "color", keyObject);
		}
		convRGBstr((char *)json_object_get_string(keyObject), &displayDate.red, &displayDate.green, &displayDate.blue);

		if (!json_object_object_get_ex(rootObject, "timezone", &keyObject))
		{
			keyObject = json_object_new_string(getenv("TZ"));
			json_object_object_add(rootObject, "timezone", keyObject);
		}
		displayDate.timezone = json_object_get_string(keyObject);
	}
	// printf("JSON(date):%s\n", json_object_get_string(rootObject));


	//从JSON中读取其他配置 ConfigJSON.text[] => displayText[] 
	if (json_object_object_get_ex(ConfigJSON, "text", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "format", &keyObject))
		{
			keyObject = json_object_new_string("");
			json_object_object_add(rootObject, "format", keyObject);
		}
		displayText.format = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("/usr/local/share/fonts/rgbmatrix/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayText.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "font", &keyObject))
		{
			keyObject = json_object_new_string("6x9.bdf");
			json_object_object_add(rootObject, "font", keyObject);
		}
		displayText.fontname = json_object_get_string(keyObject);
		fetchFont(&displayText);

		if (!json_object_object_get_ex(rootObject, "x", &keyObject))
		{
			keyObject = json_object_new_int(0);
			json_object_object_add(rootObject, "x", keyObject);
		}
		displayText.x = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "y", &keyObject))
		{
			keyObject = json_object_new_int(30);
			json_object_object_add(rootObject, "y", keyObject);
		}
		displayText.y = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(0);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayText.show = json_object_get_boolean(keyObject);

		if (!json_object_object_get_ex(rootObject, "color", &keyObject))
		{
			keyObject = json_object_new_string("888888");
			json_object_object_add(rootObject, "color", keyObject);
		}
		convRGBstr((char *)json_object_get_string(keyObject), &displayText.red, &displayText.green, &displayText.blue);

		displayText.timezone = "";	// Not used for plain text.
	}
	// printf("JSON(date):%s\n", json_object_get_string(rootObject));
	*/


	rgb_matrix::RuntimeOptions runtime_opt;
	if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &panelOptions, &runtime_opt))
	{
		return usage(argv[0]);
	}

	// Prepare matrix
	glbCanvas = CreateMatrixFromOptions(panelOptions, runtime_opt);
	if (glbCanvas == NULL)
		return 1;

	offscreen_canvas = glbCanvas->CreateFrameCanvas();
	printf("Panel size: %dx%d. Hardware gpio mapping: %s\n", glbCanvas->width(), glbCanvas->height(), panelOptions.hardware_mapping);

	return 0;
}

// ################################################################################



void start_engine_from_queue(struct chooQ* pQ ){
	//从q中取msg
	msg_t *msg_item;
	if(DEQUEUE_RESULT_SUCCESS == chooQ_dequeue_ptr(pQ, &msg_item))
		Tween_StartTween(msg_item->tween, GetTimeInMillis());	//并运行第一个msg的tween

}

//msg tween例行更新
void update_msg_draw(Tween* tween) {
	msg_t *msg_item = (msg_t*)tween->data;
	Color color(0, 60, 0);
	rgb_matrix::DrawText(offscreen_canvas, glbFont, (int)tween->props.x, 24, color, NULL, msg_item->txt, 0);
}

//msg tween正式开始 回调
void start_msg_tween(Tween* tween) {

}

//msg tween 结束 回调
void complete_msg_tween(Tween* tween){

//if(ENGINE_IS_IDLE(glbEngine))	start_engine_from_queue(&chooQueue);

}




void connect_callback(struct mosquitto *mosq, void *obj, int result){
	printf("Connect_callback(), rc=%d --------------->\n", result);

	setTimeoutConnected();	//set online and reset timeout


	char channel[128];
	sprintf(channel, "%s/#", glbMqttCfg.root);
	//printf("mosquitto_subscribing...\n");
	mosquitto_subscribe(mosq, NULL, channel, 0);
	printf("mosquitto_subscribe[\"%s\"] done.\n", channel);

}


void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message){
	bool match = false;
	//int payloadInt = 0x00;
	// int payloadInt = std::stoi((char *)message->payload, nullptr, 0);
	//if (message->payloadlen > 0)
	//	payloadInt = strtol((char *)message->payload, nullptr, 0);

	//printf("got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

	char channel[128];
	sprintf(channel, "%s/msg", glbMqttCfg.root);

	//匹配msg类型
	mosquitto_topic_matches_sub(channel, message->topic, &match);
	if (match)
	{
		msg_t *msg_item;
		if(ENQUEUE_RESULT_SUCCESS == chooQ_enqueue_alloc(&chooQueue, &msg_item)){

			//拷贝 msg ，如果过长会截断
			int chars = strncpy_utf8((char*)message->payload, msg_item->txt, MSG_LENGTH_MAX);
			if(chars == MSG_LENGTH_MAX)
				printf("Message exceeded %d chars.Trimmed.\n", MSG_LENGTH_MAX);

			printf("Topic:%s[%d] insert: %s.\n", message->topic, chooQ_length(&chooQueue), msg_item->txt);

			//根据字符串 计算输出信息的图像宽度。借助DrawText()函数，往y=-512 地方画入，不会造成实际绘制，仅仅计算图像长度
			Color color(0, 0, 0);
			int width = rgb_matrix::DrawText(offscreen_canvas, glbFont, 0, -512, color, NULL, msg_item->txt, 0);

			//通过 字符串图像的长度 和 屏幕长度，来设计 tween 并挂载到 msg 对象上
			Tween_Props props = Tween_MakeProps(glbCanvas->width(), 0, 0, 0, 0);	//从屏幕右边缘
			Tween_Props toProps = Tween_MakeProps(-width, 0, 0, 0, 0);	//到左侧完全隐没
			int move = glbCanvas->width() + width; 
			int time = move * glbCanvas->width() / 2000;	//以屏幕宽度运动2s为基础，计算运动时间
			msg_item->tween = Tween_CreateTween(glbEngine, &props, &toProps, time, TWEEN_EASING_LINEAR, update_msg_draw, msg_item);	
			msg_item->tween->startCallback = start_msg_tween;
			msg_item->tween->completeCallback = complete_msg_tween;

			if(ENGINE_IS_IDLE(glbEngine))
				start_engine_from_queue(&chooQueue);

			
		}
		else{
			printf("Message queue is full, topic:%s drop.\n", message->topic);
		}
	}


}

int initMQTT(void){
	// uint8_t reconnect = true;
	//char clientid[24];
	int rc = 0;

	//const tmillis_t old = GetTimeInMillis();

	mosquitto_lib_init();

	//memset(clientid, 0, 24);
	//snprintf(clientid, 23, "mysql_log_%d", getpid());

	mosq = mosquitto_new(glbMqttCfg.clientid, true, 0);
	if(mosq)
	{
		mosquitto_connect_callback_set(mosq, connect_callback);
		mosquitto_message_callback_set(mosq, message_callback);

		printf("mosquitto_connecting...\n");
	
		setTimeoutConnecting();	//set reset timeout
		rc = mosquitto_connect(mosq, glbMqttCfg.host, glbMqttCfg.port, 60);
		//printf("mosquitto_connect issued...cost %dms\n", (int)(GetTimeInMillis()-old));
	}

	return(rc);
}


int runMQTT(void){
	int rc = 0;
	//tmillis_t old = GetTimeInMillis();

	// MQTT client.
	if(mosq)
	{
		//rc = mosquitto_loop(mosq, -1, 1);
		rc = mosquitto_loop(mosq, 0, 1);
		//printf("mosquitto_loop() cost %dms\n", (int)(GetTimeInMillis()-old));
		if(!Interrupt && rc)
		{

			/*
			switch (rc){
				case MOSQ_ERR_SUCCESS:
					printf("mosquitto_loop ret: MOSQ_ERR_SUCCESS\n");
					break;
				case MOSQ_ERR_INVAL:
					printf("mosquitto_loop ret: MOSQ_ERR_INVAL\n");
					break;
				case MOSQ_ERR_NOMEM:
					printf("mosquitto_loop ret: MOSQ_ERR_NOMEM\n");
					break;
				case MOSQ_ERR_PROTOCOL:
					printf("mosquitto_loop ret: MOSQ_ERR_PROTOCOL\n");
					break;
				case MOSQ_ERR_ERRNO:
					printf("mosquitto_loop ret: MOSQ_ERR_ERRNO\n");
					break;
				case MOSQ_ERR_NO_CONN:
					printf("mosquitto_loop ret: MOSQ_ERR_NO_CONN\n");
					break;
				case MOSQ_ERR_CONN_LOST:
					printf("mosquitto_loop ret: MOSQ_ERR_CONN_LOST\n");
					break;

				default :
					printf("mosquitto_loop ret: %d\n", rc);
			}
			*/

			if(MOSQ_ERR_NO_CONN == rc || MOSQ_ERR_CONN_LOST == rc)
				setOffline();

			//不在线 并且 等候时间足够
			if( !isOnline() && isTimeoutForConnect()){
				printf("reconnecting...\n");
				setTimeoutConnecting();	//set reset timeout
				int rt = mosquitto_reconnect_async(mosq);
				printf("reconnection issued...ret:%d\n", rt);
			}
		}
	}

	return(rc);
}


void update(Tween* tween) {
    //Square* square;
    //square  = (Square*)tween->data;
	char buf[128];
	printf("\r");
	int i;
	for(i = 0; i < 100; i++)
		buf[i] = i == (int)tween->props.x? '*':'-';
	buf[i] = '\0';
	printf("%s", buf);
}

int main(int argc, char *argv[]){



	signal(SIGTERM, InterruptHandler);
	signal(SIGINT, InterruptHandler);

	const char *filename = "config.json";
	int fileJSON = open(filename, O_RDONLY, 0);
	if (fileJSON < 0)
	{
		fprintf(stderr, "FAIL: unable to open %s: %s\n", filename, strerror(errno));
		exit(0);
	}
	close(fileJSON);

	printf("Reading JSON config file '%s'...", filename);
	ConfigJSON = json_object_from_file(filename);
	if (ConfigJSON != NULL)
	{
		printf("Done\n");
		//printf("OK: json_object_from_fd(%s)=%s\n", filename, json_object_to_json_string(ConfigJSON));
	}
	else
	{
		fprintf(stderr, "FAIL: unable to parse contents of %s: %s\n", filename, json_object_to_json_string(ConfigJSON));
		exit(1);
	}

	printf("Loading font...");
	int rt = glbFont.LoadFont("msyh24.bdf");
	if (!rt)
	{
		printf("Done\n");
	}
	else
	{
		fprintf(stderr, "FAIL: unable to load font: %s\n", "msyh24.bdf");
		exit(1);
	}


	initPanel(argc, argv);
	chooQ_init(&chooQueue);	//初始化队列

	initMQTT();


    glbEngine = Tween_CreateEngine();
   


    Tween_Props props = Tween_MakeProps(0, 0, 0, 0, 0);
    Tween_Props toProps = Tween_MakeProps(100, 0, 0, 0, 0);

    Tween* tween = Tween_CreateTween(glbEngine, &props, &toProps, 3000, TWEEN_EASING_BOUNCE_OUT, update, NULL);
    tween->delay = 3000;
    //tweenBack = Tween_CreateTween(glbEngine, &toProps, &props, 3000, TWEEN_EASING_ELASTIC_IN_OUT, update, &square);
    //Tween_ChainTweens(tween, tweenBack);
    //Tween_ChainTweens(tweenBack, tween);
    Tween_StartTween(tween, GetTimeInMillis());



	while(!Interrupt)
	{
		for(int i = 0; i < 200; i++){
			int vsync_multiple = 1;
			DisplayAnimation(glbCanvas, offscreen_canvas, vsync_multiple); //1000 frames per second
	        
			Tween_UpdateEngine(glbEngine, GetTimeInMillis());

			SleepMillis(5);	//
		}
		runMQTT();	//10 times per second.
		//spinning();
	}


    Tween_DestroyTween(tween);
    Tween_DestroyEngine(glbEngine);
	glbEngine = NULL;

	mosquitto_lib_cleanup();

	// Animation finished. Shut down the RGB canvas.
	glbCanvas->Clear();
	delete glbCanvas;

	if (Interrupt)
	{
		fprintf(stderr, "Caught signal. Exiting.\n");
	}
	printf("\nEnded.\n\n");

	return 0;

}



void DisplayAnimation(RGBMatrix *matrix, FrameCanvas *offscreen_canvas, int vsync_multiple)
{

	offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, vsync_multiple);

}