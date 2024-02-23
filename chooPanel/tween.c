/**
 * libtween
 * Copyright (C) 2013 libtween authors.
 *
 * Based on tween.js Copyright (c) 2010-2012 Tween.js authors.
 * Easing equations Copyright (c) 2001 Robert Penner http://robertpenner.com/easing/
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tween.h"

/**
 * Tween_CreateEngine() 这个代码很简单,就是malloc结构出来。
 */
Tween_Engine* Tween_CreateEngine() {
    Tween_Engine* engine;

    engine = (Tween_Engine*) malloc(sizeof(Tween_Engine));
    memset(engine, 0, sizeof(Tween_Engine));
    
    return engine;
}

/**
 * Tween_DestroyEngine() 代码简单，销毁engine及其挂载链表
 */

void Tween_DestroyEngine(Tween_Engine* engine) {
    Tween_Node* node;
    node = engine->tweens;
 
    while (node) {
        Tween_Node* nextNode = node->next;
        free(node);
        node = nextNode;
    }
    
    free(engine);
}

/**
 * Tween_UpdateEngine() 重要函数，随时间重复调用，其链表tween对象也逐一被调用更新时间
 */

int Tween_UpdateEngine(Tween_Engine* engine, uint32_t time) {
    Tween_Node* node;
    Tween_Node* tempNode = NULL;

    node = engine->tweens;

    if (!node) {
        return 0;
    }
    
    while (node) {
        if (!Tween_UpdateTween(node->tween, time)) {
            if (tempNode) {
                tempNode->next = node->next;
                free(node);
                node = tempNode->next;
            }
            else {
                engine->tweens = node->next;
                free(node);
                node = engine->tweens;
            }
        }
        else {
            tempNode = node;
            node = node->next;
        }
    }
    
    return 1;
}

/**
 * Tween_CreateTween()
 */

Tween* Tween_CreateTween(Tween_Engine* engine, Tween_Props* props, Tween_Props* toProps, uint32_t duration, Tween_Easing easing, Tween_Callback updateCallback, void* data) {
    Tween* tween;

    tween = (Tween*) malloc(sizeof(Tween));
    memset(tween, 0, sizeof(Tween));
    
    tween->engine = engine;
    
    Tween_CopyProps(props, &tween->props);
    Tween_CopyProps(toProps, &tween->toProps);

    Tween_CopyProps(props, &tween->startProps);
    Tween_CopyProps(props, &tween->repeatProps);    

    tween->duration = duration;
    tween->easing = easing;
    tween->updateCallback = updateCallback;
    tween->data = data;
    
    return tween;
}

/**
 * Tween_CreateTweenEx()
 */

Tween* Tween_CreateTweenEx(Tween_Engine* engine, Tween_Props* props, Tween_Props* toProps, uint32_t duration, uint32_t delay, int repeat, int yoyo, Tween_Easing easing, Tween_Callback startCallback, Tween_Callback updateCallback, Tween_Callback completeCallback, void* data) {
    Tween* tween;

    tween = Tween_CreateTween(engine, props, toProps, duration, easing, updateCallback, data);
    
    tween->delay = delay;
    tween->repeat = repeat;
    tween->yoyo = yoyo;
    tween->startCallback = startCallback;
    tween->completeCallback = completeCallback;
    
    return tween;
}

/**
 * Tween_ChainTweens()
 */

void Tween_ChainTweens(Tween* tween, Tween* tween2) {
    Tween_Node* node;
    Tween_Node* tempNode;

    node = (Tween_Node*) malloc(sizeof(Tween_Node));
    memset(node, 0, sizeof(Tween_Node));

    node->tween = tween2;
    
    if (tween->chain == NULL) {
        tween->chain = node;
    }
    else {
        tempNode = tween->chain;

        while(tempNode->next) {
            tempNode = tempNode->next;
        }

        tempNode->next = node;
    }
}

/**
 * Tween_DestroyTween()  销毁tween。看起来tween都是用户构造，所以销毁也全部只能用户调用该函数完成。lib没有任何销毁tween的动作，仅仅会创建销毁node结构。
 */

void Tween_DestroyTween(Tween* tween) {
    Tween_Node* node;
    Tween_Node* tempNode;

    node = tween->chain;    //从chain中 仅仅销毁 Tween_Node 不销毁附带的tween
    
    while (node) {
        tempNode = node->next;
        free(node);

        node = tempNode;
    }
    
    free(tween);    //本身tween销毁掉
}

/**
 * Tween_StartTween() 重要函数！！！  启动一个tween。malloc一个node，借助其挂到engine的链表上去
 */

void Tween_StartTween(Tween* tween, uint32_t time) {
    Tween_Node* node;
    Tween_Node* tempNode;

    tween->startTime = time;    //时间设置为当前
    tween->startTime += tween->delay;
    
    Tween_CopyProps(&tween->startProps, &tween->props);
    
    node = (Tween_Node*) malloc(sizeof(Tween_Node));    //malloc一个node
    memset(node, 0, sizeof(Tween_Node));

    node->tween = tween;    //下面该tween借助node 挂到engine的链表尾部
    
    if (!tween->engine->tweens) {
        tween->engine->tweens = node;
    }
    else {
        tempNode = tween->engine->tweens;

        while(tempNode->next) {
            tempNode = tempNode->next;
        }

        tempNode->next = node;
    }
}

/**
 * Tween_StopTween() 很简单，就是从engine中摘下来，销毁其node。注意，没有销毁tween本身！！
 */

void Tween_StopTween(Tween* tween) {
    Tween_Node* node;
    Tween_Node* tempNode = NULL;

    node = tween->engine->tweens;

    //下面摘出含有tween的node，销毁node，注意，tween不销毁    
    while (node) {
        if (node->tween == tween) {
            if (tempNode) { //tempNode不是根节点
                tempNode->next = node->next;
            }
            else {  //tempNode是根节点 
                tween->engine->tweens = node->next;
            }
            
            free(node);
            break;
        }
        else {
            tempNode = node;
            node = node->next;
        }
    }
}

/**
 * Tween_UpdateTween() 重要函数，一个tween的时间更新。
 */

int Tween_UpdateTween(Tween* tween, uint32_t time) {
    double elapsed; 
    Tween_Node* node;

    //时间未到 返回1
    if (time < tween->startTime) {
        return 1;
    }
    
    //根据tween开始标记，做第一次行动。
    if (!tween->startCallbackFired) {
        if (tween->startCallback) {
            tween->startCallback(tween);
        }
        
        tween->startCallbackFired = 1;
    }
    
    //下面根据现在时间 和 持续时间，计算出一个0～1之间的小数，表达进度。如此，可以套用所有easing function的输入范畴
    elapsed = (double)(time - tween->startTime) / (double)tween->duration;
    elapsed = (elapsed > 1) ? 1 : elapsed;  //时间过了，就直接到结尾1
    
    //-------------------------------------------------------
    // 最重要过程，调用easing function 计算出 当前值 （0～1）并更新到prop中
    //-------------------------------------------------------
    Tween_UpdateProps(&tween->startProps, &tween->toProps, &tween->props, tweenEasingFuncs[tween->easing](elapsed));
    
    //prop 更新后，就可以调用 UI层面逻辑 更新画面了
    if (tween->updateCallback) {
        tween->updateCallback(tween);
    }
    
    
    if (elapsed == 1) { //如果结束了
        if (tween->repeat > 0) {    //循环tween
            tween->repeat--;
            
            if (tween->yoyo) {  //
                Tween_SwapProps(&tween->repeatProps, &tween->toProps);  //repeatProps 初始化时是起始位置，这里交换，正好掉个
                tween->reversed = !tween->reversed; //标记下
            }
            
            Tween_CopyProps(&tween->repeatProps, &tween->startProps); //这里 repeatProps => startProps  有点不清楚为啥，因为 后者好像从来没变过
            tween->startTime = time + tween->delay; //更新 startTime 
            
            return 1;   //
        }

        //如果结束了 而且不是循环tween，那么开始执行该tween本身的chain里面的其他tween
        else {
            node = tween->chain;
            //
            while (node) {
                Tween_StartTween(node->tween, time);    //该tween.chain下的所有tween都开始（挂到engine上去）。
                node = node->next;
            }

            //执行当前tween结束回调
            if (tween->completeCallback) {
                tween->completeCallback(tween);
            }
            
            return 0;
        }
    }
    
    return 1;
}
