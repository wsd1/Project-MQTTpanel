

typedef int64_t tmillis_t;


int get_local_mac(const char *eth_inf, char *mac); // 获取本机mac
int get_local_ip(const char *eth_inf, char *ip); // 获取本机ip

bool convRGBstr(char *str, uint8_t *red, uint8_t *green, uint8_t *blue);

void spinning();

tmillis_t GetTimeInMillis();
void SleepMillis(tmillis_t milli_seconds);

int utf8_bytes(const char* it);
int strncpy_utf8(const char* str, char* dst, int n);
int strlen_utf8(const char* str);


