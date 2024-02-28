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
#include "utils.h"


#define mqtt_host "192.168.31.79"
#define mqtt_port 1883
#define RECONNECT_TIMEOUT 10000
#define MSG_LENGTH_MAX 128

int sflags = 0;
#define json_object_to_json_string(obj) json_object_to_json_string_ext(obj, sflags)


//using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;


// ################################################################################

typedef struct
{
	char root[64];
	char clientid[64];
	char host[64];
	uint16_t port;
} mqttCfg_t;





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
RGBMatrix::Options matrix_options;
rgb_matrix::RuntimeOptions runtime_opt;

RGBMatrix *canvas;
FrameCanvas *glbOffscreen_canvas;
rgb_matrix::Font glbFont;

mqttCfg_t mqtt_options;
volatile bool Interrupt = false;

tmillis_t glbLastConnection = 0;
bool glbIsOnline = false;

// ################################################################################

bool is_canvas_dirty = false;

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


// 在 ConfigJSON.panel对象中检查Key，不存在则用 matrix_options[key] 值创建；
// 如果 JSONpref == true，那么 matrix_options[key] 设置为json中的值；
void setPanelConfig(json_object *rootObject, RGBMatrix::Options *pMatrixOption, char *Key, bool Value, bool JSONpref){
	json_object *panelObject;
	if (!json_object_object_get_ex(rootObject, "panel", &panelObject))
		return;

	//上面在全局结构变量 matrix_options 中找到的相应key 和 value值
	bool *ptrKey;
	if (strcmp(Key, "show_refresh_rate") == 0)
		ptrKey = &pMatrixOption->show_refresh_rate;

	else if (strcmp(Key, "inverse_colors") == 0)
		ptrKey = &pMatrixOption->inverse_colors;

	else
		return;

	//json结构panel对象下面，如果没有就创建（值继承matrix_options.key）
	json_object *keyObject;
	if (!json_object_object_get_ex(panelObject, Key, &keyObject))	
	{
		keyObject = json_object_new_boolean((json_bool)(*ptrKey));
		json_object_object_add(panelObject, Key, keyObject);
	}

	//函数参数JSONpref 若为true，则 matrix_options.key 获取json对象中相应key值
	if (JSONpref == true)
		(*ptrKey) = json_object_get_boolean(keyObject);
	else
		(*ptrKey) = Value;
}

void setPanelConfig(json_object *rootObject, RGBMatrix::Options *pMatrixOption, char *Key, char *Value, bool JSONpref){
	json_object *panelObject;
	if (!json_object_object_get_ex(rootObject, "panel", &panelObject))
		return;
	char *ptrKey;
	if (strcmp(Key, "hardware_mapping") == 0)
		ptrKey = (char *)pMatrixOption->hardware_mapping;
	else if (strcmp(Key, "led_rgb_sequence") == 0)
		ptrKey = (char *)pMatrixOption->led_rgb_sequence;
	else
		return;


	json_object *keyObject;
	if (!json_object_object_get_ex(panelObject, Key, &keyObject))
	{
		keyObject = json_object_new_string(ptrKey);
		json_object_object_add(panelObject, Key, keyObject);
	}
	if (JSONpref == true)
		strcpy((char *)json_object_get_string(keyObject), ptrKey);
	else
		strcpy(Value, ptrKey);
}

void setPanelConfig(json_object *rootObject, RGBMatrix::Options *pMatrixOption, char *Key, int Value, bool JSONpref){
	json_object *panelObject;
	if (!json_object_object_get_ex(rootObject, "panel", &panelObject))
		return;

	int *ptrKey;
	if (strcmp(Key, "rows") == 0)
		ptrKey = &pMatrixOption->rows;
	else if (strcmp(Key, "cols") == 0)
		ptrKey = &pMatrixOption->cols;
	else if (strcmp(Key, "chain_length") == 0)
		ptrKey = &pMatrixOption->chain_length;
	else if (strcmp(Key, "parallel") == 0)
		ptrKey = &pMatrixOption->parallel;
	else if (strcmp(Key, "multiplexing") == 0)
		ptrKey = &pMatrixOption->multiplexing;
	else if (strcmp(Key, "brightness") == 0)
		ptrKey = &pMatrixOption->brightness;
	else if (strcmp(Key, "scan_mode") == 0)
		ptrKey = &pMatrixOption->scan_mode;
	else if (strcmp(Key, "pwm_bits") == 0)
		ptrKey = &pMatrixOption->pwm_bits;
	else if (strcmp(Key, "pwm_lsb_nanoseconds") == 0)
		ptrKey = &pMatrixOption->pwm_lsb_nanoseconds;
	else if (strcmp(Key, "row_address_type") == 0)
		ptrKey = &pMatrixOption->row_address_type;
	else if (strcmp(Key, "limit_refresh_rate_hz") == 0)
		ptrKey = &pMatrixOption->limit_refresh_rate_hz;
	else
		return;

	json_object *keyObject;
	if (!json_object_object_get_ex(panelObject, Key, &keyObject)){
		keyObject = json_object_new_int((int32_t)(*ptrKey));
		json_object_object_add(panelObject, Key, keyObject);
	}
	if (JSONpref == true)
		(*ptrKey) = json_object_get_int(keyObject);
	else
		(*ptrKey) = Value;
}

// ConfigJSON.panel[key] => matrix_options[key] 
// JSON => struct
void setPanelConfig(json_object *rootObject, RGBMatrix::Options *pMatrixOption, char *Key){	
	if (strcmp(Key, "show_refresh_rate") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, true, true);
	else if (strcmp(Key, "inverse_colors") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, true, true);
	else if (strcmp(Key, "hardware_mapping") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (char *)NULL, true);
	else if (strcmp(Key, "led_rgb_sequence") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (char *)NULL, true);
	else if (strcmp(Key, "rows") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "cols") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "chain_length") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "parallel") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "multiplexing") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "brightness") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "scan_mode") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "pwm_bits") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "pwm_lsb_nanoseconds") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "row_address_type") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);
	else if (strcmp(Key, "limit_refresh_rate_hz") == 0)
		setPanelConfig(rootObject, pMatrixOption, Key, (int)0, true);



}

// ################################################################################


int init_panel_from_json(json_object *rootObject, RGBMatrix::Options *pMatrixOption){
	// ConfigJSON[panel] => matrix_options[key] 
	setPanelConfig(rootObject, pMatrixOption, (char *) "hardware_mapping");
	setPanelConfig(rootObject, pMatrixOption, (char *) "led_rgb_sequence");
	setPanelConfig(rootObject, pMatrixOption, (char *) "rows");
	setPanelConfig(rootObject, pMatrixOption, (char *) "cols");
	setPanelConfig(rootObject, pMatrixOption, (char *) "chain_length");
	setPanelConfig(rootObject, pMatrixOption, (char *) "parallel");
	setPanelConfig(rootObject, pMatrixOption, (char *) "multiplexing");
	setPanelConfig(rootObject, pMatrixOption, (char *) "brightness");
	setPanelConfig(rootObject, pMatrixOption, (char *) "scan_mode");
	setPanelConfig(rootObject, pMatrixOption, (char *) "pwm_bits");
	setPanelConfig(rootObject, pMatrixOption, (char *) "pwm_lsb_nanoseconds");
	setPanelConfig(rootObject, pMatrixOption, (char *) "row_address_type");
	setPanelConfig(rootObject, pMatrixOption, (char *) "show_refresh_rate");
	setPanelConfig(rootObject, pMatrixOption, (char *) "inverse_colors");
	setPanelConfig(rootObject, pMatrixOption, (char *) "limit_refresh_rate_hz");
	

	return 0;
}

int get_mqtt_cfg_from_json(json_object *pRootNode, mqttCfg_t *pMqttCfg){
	
	json_object *mqttObject;

	//从JSON中读取其他配置 ConfigJSON[mqtt] => mqttObject
	if (json_object_object_get_ex(pRootNode, "mqtt", &mqttObject))	{
		json_object *keyObject;

		if (!json_object_object_get_ex(mqttObject, "clientid", &keyObject)){
			keyObject = json_object_new_string("defaultID:chooPanel");
			json_object_object_add(mqttObject, "clientid", keyObject);
		}
		strcpy(pMqttCfg->clientid, json_object_get_string(keyObject));

		if (!json_object_object_get_ex(mqttObject, "host", &keyObject)){
			keyObject = json_object_new_string("mqtt.eclipseprojects.io");	//paho.mqtt.c demo broker
			json_object_object_add(mqttObject, "host", keyObject);
		}
		strcpy(pMqttCfg->host, json_object_get_string(keyObject));

		if (!json_object_object_get_ex(mqttObject, "port", &keyObject)){
			keyObject = json_object_new_int(1883);
			json_object_object_add(mqttObject, "port", keyObject);
		}
		pMqttCfg->port = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(mqttObject, "root", &keyObject)){
			keyObject = json_object_new_string("/chooPanel");
			json_object_object_add(mqttObject, "root", keyObject);
		}
		strcpy(pMqttCfg->root, json_object_get_string(keyObject));
		return 0;
	}
	return -1;

}

// ################################################################################

void play_msg_tween_from_queue(struct chooQ* pQ ){
	//从q中取msg
	int msg_remain = chooQ_length(pQ);
	if(msg_remain > 0){
		msg_t *msg_item = NULL;
		//printf("__debug__: <--- Queue[%d]. Get one.\n", msg_remain);
		chooQ_dequeue_ptr(pQ, &msg_item);
		//printf("__debug__: Qi:%d, Qo:%d. pMSG:[0x%08x], MSG.txt:%s. MSG.tween:0x%08x\n", pQ->write_idx, pQ->read_idx, (unsigned int)msg_item, msg_item->txt, (unsigned int)msg_item->tween);

		Tween_StartTween(msg_item->tween, GetTimeInMillis());	//并运行第一个msg的tween
	}
	else{
		//printf("__debug__: Try get msg, but empty....\n");
	}
}

//msg tween例行更新
void update_msg_draw(Tween* tween) {
	msg_t *msg_item = (msg_t*)tween->data;

	Color color((int)tween->props.r, (int)tween->props.g, (int)tween->props.b);
	//Color color(150,20,150);
	rgb_matrix::DrawText(glbOffscreen_canvas, glbFont, (int)tween->props.x, 24, color, NULL, msg_item->txt, 0);

	is_canvas_dirty = true;	//标记 有新内容了，等会把fb换掉
}

//msg tween开始；为msg更新当前tween。
void cb_tween_start(Tween* tween) {
	msg_t *msg_item = (msg_t*)tween->data;

	//msg对象 挂一串tween,其tween指针仅仅指向链表头.
	//随着tween执行完成,销毁脱落. msg->tween 若保持指向链表头，存在风险。
	//所以,在每个tween执行开始,都更新一下当前msg的tween
	msg_item->tween = tween;
	//printf("__debug__: <<<<--------------- tween[%s] start...\n", msg_item->txt);
}

//tween 结束，为msg清理当前tween, 销毁自己
void cb_tween_complete_and_destroy(Tween* tween){
	msg_t *msg_item = (msg_t*)tween->data;
	//printf("__debug__: --------------->>>> tween[%s] finish...\n", msg_item->txt);

	//清除将要free的指针
	msg_item->tween = NULL;
	Tween_DestroyTween(tween); //销毁自己
}


int handle_msg(const char* payload){

	msg_t *msg_item;
	if(ENQUEUE_RESULT_SUCCESS == chooQ_enqueue_alloc(&chooQueue, &msg_item)){

		//拷贝 msg ，如果过长会截断
		int chars = strncpy_utf8(payload, msg_item->txt, MSG_LENGTH_MAX);
		if(chars == MSG_LENGTH_MAX)
			printf("Get long msg, Trimmed to %d chars.\n", MSG_LENGTH_MAX);

		//printf("Topic:%s[%d] insert: %s.\n", message->topic, chooQ_length(&chooQueue), msg_item->txt);

		//根据字符串 计算输出信息的图像宽度。借助DrawText()函数，往y=-512 地方画入，不会造成实际绘制，仅仅计算图像长度
		Color color(0, 0, 0);
		int msg_width = rgb_matrix::DrawText(glbOffscreen_canvas, glbFont, 0, -512, color, NULL, msg_item->txt, 0);

		//通过 字符串图像的长度 和 屏幕长度，来设计 tween 并挂载到 msg 对象上
		//printf("__debug__: get msg. msg image width: %d.canvas->width: %d. tween time:%d \n", msg_width, canvas->width(), time);
		int canvas_width = canvas->width(), firstStop = 0;
		float msPerPixel = 3000 / canvas_width;	//以屏幕宽度运动3s为基础，计算运动速度 (重要，这里计算出标准速度，后面全部的动画时间计算都是以此作为标准的)
		Tween_Props props, toProps;
		Tween *ptween_head = NULL, *ptween_pre = NULL, *ptween = NULL;

		if(msg_width < canvas_width){	//如果消息能全屏容纳，那么先停留在对中位置
			
			//到对中
			firstStop = (canvas_width - msg_width)/2;			
			props = Tween_MakeProps(canvas_width, 0, 0, 0, 0);	//从屏幕右边缘;
			props.r = 20; props.g = 20; props.b = 200; 

			Tween_CopyProps(&props, &toProps);
			toProps.x = firstStop;
			toProps.r += 180; toProps.g += 0; toProps.b += -180; 
			ptween_head = ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)((canvas_width - firstStop)*msPerPixel), TWEEN_EASING_BACK_OUT, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;


			//停留
			Tween_CopyProps(&toProps, &props);
			toProps.r += -180; toProps.g += 180; toProps.b += 0; 
			ptween = Tween_CreateTween(glbEngine, &props, &toProps, 150, TWEEN_EASING_LINEAR, update_msg_draw, msg_item);
			ptween->repeat = 11; ptween->yoyo = true; 
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;

			//移出
			Tween_CopyProps(&toProps, &props);
			toProps.x = -msg_width;	//到消息消失
			toProps.r += 0; toProps.g += -180; toProps.b += 180; 
			ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)((firstStop + msg_width)*msPerPixel), TWEEN_EASING_BACK_IN, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;
		}
		//比较长 但是没有超出两屏
		else if(msg_width < 2 * canvas_width){
			//先停留在左侧边缘
			firstStop = 0;
			props = Tween_MakeProps(canvas_width, 0, 0, 0, 0);	//从屏幕右边缘;
			props.r = 200; props.g = 20; props.b = 20; 

			Tween_CopyProps(&props, &toProps);
			toProps.x = firstStop;	//到左边缘
			toProps.r += -180; toProps.g += 180; toProps.b += 0;
			ptween_head = ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)(canvas_width*msPerPixel), TWEEN_EASING_CUBIC_OUT, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;

			//停留变色
			Tween_CopyProps(&toProps, &props);
			toProps.r += 0; toProps.g += -180; toProps.b += 180;
			ptween = Tween_CreateTween(glbEngine, &props, &toProps, 1500, TWEEN_EASING_LINEAR, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;

			//然后加速离开
			Tween_CopyProps(&toProps, &props);
			toProps.x = -msg_width;	//向左加速离开 msg消失
			toProps.r += 180; toProps.g += 0; toProps.b += -180; 
			ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)(msg_width*msPerPixel), TWEEN_EASING_CUBIC_IN, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;
		}
		//如果消息过长，那么分三段：
		else{
			//减速停留在左侧边缘
			firstStop = 0;
			props = Tween_MakeProps(canvas_width, 0, 0, 0, 0);	//从屏幕右边缘;
			props.r = 200; props.g = 20; props.b = 20; 

			Tween_CopyProps(&props, &toProps);
			toProps.x = firstStop;	//到左边缘
			toProps.r += -180; toProps.g += 180; toProps.b += 0;
			ptween_head = ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)(canvas_width*msPerPixel), TWEEN_EASING_CUBIC_OUT, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;

			//半屏加速
			int move = canvas_width/2; 
			Tween_CopyProps(&toProps, &props);
			toProps.r += 0; toProps.g += -180; toProps.b += 180;
			toProps.x -= move;
			//qubicIn的末端速度是单位速度的3倍，所以时间增到3倍，以保证末端速度能是正常的匀速
			ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)(move*msPerPixel*3), TWEEN_EASING_CUBIC_IN, update_msg_draw, msg_item);	//TWEEN_EASING_LINEAR TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;

			//然后匀速离开
			Tween_CopyProps(&toProps, &props);
			toProps.x = -msg_width;	//向左离开 直到msg消失
			toProps.r += 180; toProps.g += 0; toProps.b += -180; 
			ptween = Tween_CreateTween(glbEngine, &props, &toProps, (int)((msg_width-move)*msPerPixel), TWEEN_EASING_LINEAR, update_msg_draw, msg_item);	// TWEEN_EASING_BACK_IN_OUT
			//处理新构造tween，加装回调，并链接
			ptween->startCallback = cb_tween_start;
			ptween->completeCallback = cb_tween_complete_and_destroy;
			if(NULL!= ptween_pre)Tween_ChainTweens(ptween_pre, ptween);
			ptween_pre = ptween;

		}		

		//msg结构 挂上tween头
		msg_item->tween = ptween_head;

		

		//printf("__debug__: --->Qi:%d, Qo:%d. Queue[%d], pMsg[0x%08x], Msg[%s], Tween[0x%08x] incoming. \n", chooQueue.write_idx, chooQueue.read_idx, chooQ_length(&chooQueue), (unsigned int)msg_item, msg_item->txt, (unsigned int)msg_item->tween);
		return 0;
	}
	else{
		printf("Message queue is full, drop.\n");
		return -1;
	}

}

void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message){
	bool match = false;
	//int payloadInt = 0x00;
	// int payloadInt = std::stoi((char *)message->payload, nullptr, 0);
	//if (message->payloadlen > 0)
	//	payloadInt = strtol((char *)message->payload, nullptr, 0);

	//printf("got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

	char channel[128];
	sprintf(channel, "%s/msg", mqtt_options.root);

	//匹配msg类型
	mosquitto_topic_matches_sub(channel, message->topic, &match);
	if (match)
		handle_msg((char*)message->payload);

}

void connect_callback(struct mosquitto *mosq, void *obj, int result){
	printf("Broker connect_callback(), rc=%d\n", result);
	setTimeoutConnected();	//set online and reset timeout

	char channel[128];
	sprintf(channel, "%s/#", mqtt_options.root);
	//printf("mosquitto_subscribing...\n");
	int rt = mosquitto_subscribe(mosq, NULL, channel, 0);
	printf("mosquitto_subscribe[\"%s\"] return %d.\n", channel, rt);

	char buf[256], ip[16]; 
	get_local_ip("eth0", ip);
	sprintf(buf, "MQTT broker: %s:%d.Send message to topic '%s/msg'", ip, mqtt_options.port, mqtt_options.root);
	handle_msg(buf);
}

int initMQTT(void){
	// uint8_t reconnect = true;
	//char clientid[24];
	int rc = 0;
	//const tmillis_t old = GetTimeInMillis();
	mosquitto_lib_init();
	//memset(clientid, 0, 24);
	//snprintf(clientid, 23, "mysql_log_%d", getpid());
	mosq = mosquitto_new(mqtt_options.clientid, true, 0);
	if(mosq){
		mosquitto_connect_callback_set(mosq, connect_callback);
		mosquitto_message_callback_set(mosq, message_callback);
		printf("mosquitto_connecting...\n");
		setTimeoutConnecting();	//set reset timeout
		rc = mosquitto_connect(mosq, mqtt_options.host, mqtt_options.port, 60);
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

int main(int argc, char *argv[]){

	signal(SIGTERM, InterruptHandler);
	signal(SIGINT, InterruptHandler);

	//################
	//打开config.json
	//################
	const char *filename = "config.json";
	int fileJSON = open(filename, O_RDONLY, 0);
	if (fileJSON < 0){
		fprintf(stderr, "FAIL: unable to open %s: %s\n", filename, strerror(errno));
		exit(0);
	}
	close(fileJSON);

	printf("Reading JSON config file '%s'...", filename);
	ConfigJSON = json_object_from_file(filename);
	if (ConfigJSON != NULL){
		printf("Done\n");
		//printf("OK: json_object_from_fd(%s)=%s\n", filename, json_object_to_json_string(ConfigJSON));
		get_mqtt_cfg_from_json(ConfigJSON, &mqtt_options);
		init_panel_from_json(ConfigJSON, &matrix_options);
	}
	else{
		fprintf(stderr, "FAIL: unable to parse contents of %s: %s\n", filename, json_object_to_json_string(ConfigJSON));
		exit(1);
	}


	//################
	//加载 font
	//################
	printf("Loading font...");
	if (!glbFont.LoadFont("msyh24.bdf")){
		fprintf(stderr, "FAIL: unable to load font\n");
		exit(1);
	}
	printf("Done\n");


	// printf("JSON(date):%s\n", json_object_get_string(panelObject));
	if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_opt)){
		return usage(argv[0]);
	}
	//runtime_opt.gpio_slowdown = 2; //比1 更加稳定点

	// Prepare matrix
	canvas = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
	if (canvas == NULL)
		return 1;

	/**/
	printf("matrix options: ---------------------\r\n");
	printf("hardware_mapping:%s\n", matrix_options.hardware_mapping);
	printf("rows:%d\n", matrix_options.rows);
	printf("cols:%d\n", matrix_options.cols);
	printf("chain_length:%d\n", matrix_options.chain_length);
	printf("parallel:%d\n", matrix_options.parallel);
	printf("pwm_bits:%d\n", matrix_options.pwm_bits);
	printf("pwm_lsb_nanoseconds:%d\n", matrix_options.pwm_lsb_nanoseconds);
	printf("pwm_dither_bits:%d\n", matrix_options.pwm_dither_bits);
	printf("brightness:%d\n", matrix_options.brightness);
	printf("scan_mode:%d\n", matrix_options.scan_mode);
	printf("row_address_type:%d\n", matrix_options.row_address_type);
	printf("multiplexing:%d\n", matrix_options.multiplexing);
	printf("disable_hardware_pulsing:%s\n", matrix_options.disable_hardware_pulsing? "true" : "false");
	printf("show_refresh_rate:%s\n", matrix_options.show_refresh_rate? "true" : "false");
	printf("inverse_colors:%s\n", matrix_options.inverse_colors? "true" : "false");
	printf("led_rgb_sequence:%s\n", matrix_options.led_rgb_sequence);
	printf("pixel_mapper_config:%s\n", matrix_options.pixel_mapper_config);
	printf("panel_type:%s\n", matrix_options.panel_type);
	printf("limit_refresh_rate_hz:%d\n", matrix_options.limit_refresh_rate_hz);

	printf("runtime options: ---------------------\r\n");
	printf("gpio_slowdown:%d\n", runtime_opt.gpio_slowdown);
	printf("daemon:%d\n", runtime_opt.daemon);
	printf("drop_privileges:%d\n", runtime_opt.drop_privileges);
	printf("do_gpio_init:%s\n", runtime_opt.do_gpio_init? "true" : "false");
	printf("drop_priv_user:%s\n", runtime_opt.drop_priv_user);
	printf("drop_priv_group:%s\n", runtime_opt.drop_priv_group);
	printf("--------------------------------------\r\n");
	

	glbOffscreen_canvas = canvas->CreateFrameCanvas();
	printf("Panel size: %dx%d. Hardware gpio mapping: %s\n", canvas->width(), canvas->height(), matrix_options.hardware_mapping);

	chooQ_init(&chooQueue);	//初始化队列
	//printf("sizeof(msg_t)=%ld, sizeof(tween)=%ld\n",sizeof(msg_t), sizeof(Tween));
	//printf("Queue structure occupy %ld bytes.\n", sizeof(struct chooQ));

	initMQTT();
    glbEngine = Tween_CreateEngine();
   
	Color color(100, 0, 100);
	rgb_matrix::DrawText(glbOffscreen_canvas, glbFont, 10, 24, color, NULL, "System initializing...", 0);
	is_canvas_dirty = true;


	while(!Interrupt)
	{
		for(int i = 0; i < 20; i++){
			Tween_UpdateEngine(glbEngine, GetTimeInMillis());	//在这里完成所有 tween在offscreen上的render
	
			if(is_canvas_dirty){	//有新内容，就换。要不不换，留下之前内容
				//https://github.com/hzeller/rpi-rgb-led-matrix/blob/a3eea997a9254b83ab2de97ae80d83588f696387/include/led-matrix.h#L235C57-L235C76
				glbOffscreen_canvas = canvas->SwapOnVSync(glbOffscreen_canvas);
				glbOffscreen_canvas->Fill(0,0,0);	//清空
				is_canvas_dirty = false;
			}

			SleepMillis(5);
		}
		runMQTT();	//10 times per second.
		//spinning();

		//上面mqtt_loop中可能会收到消息，tween keep on
		if(ENGINE_IS_IDLE(glbEngine))
			play_msg_tween_from_queue(&chooQueue);	
	}


    Tween_DestroyEngine(glbEngine);
	glbEngine = NULL;

	mosquitto_lib_cleanup();

	// Animation finished. Shut down the RGB canvas.
	canvas->Clear();
	delete canvas;

	if (Interrupt)
	{
		fprintf(stderr, "Caught signal. Exiting.\n");
	}
	printf("\nEnded.\n\n");

	return 0;

}

