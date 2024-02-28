
# Choo Choo Panel

---

Add mosquitto Jsonc library,use:
```
sudo apt-get install libmosquitto-dev
sudo apt-get install libjson-c-dev

```

20240222:
这个repo借鉴了 MQTTPanel的代码。改进使用了mosquitto的client api，使其可以正确处理broker断连时的情况。

划分开了framebuffer刷新循环 和 mqtt_loop循环，使前者保持1000Hz，后者保持10Hz。既可以保证动画丝滑，又能保证mqtt不占用太多时间资源。


---
参考：

https://mosquitto.org/api/files/mosquitto-h.html

https://json-c.github.io/json-c/json-c-0.10/doc/html/json__object_8h.html

