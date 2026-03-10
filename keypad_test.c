#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int cols[3] = {5,6,13};
int rows[4] = {12,16,20,21};
char keys[4][3] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};

// Helper to set column high/low
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------- GPIO wrappers ----------------

static void gpio_output(int pin) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "raspi-gpio set %d op", pin);
    system(cmd);
}

static void gpio_input_pullup(int pin) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "raspi-gpio set %d ip pu", pin);
    system(cmd);
}

static void gpio_write(int pin, int value) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "raspi-gpio set %d %s", pin, value ? "dh" : "dl");
    system(cmd);
}

static int gpio_read(int pin) {
    char cmd[64];
    char buf[128];

    snprintf(cmd, sizeof(cmd), "raspi-gpio get %d", pin);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    fgets(buf, sizeof(buf), fp);
    pclose(fp);

    return (strstr(buf, "level=0") != NULL) ? 0 : 1;
}

int main() {
    // setup done manually via raspi-gpio before running
    for(int i=0;i<3;i++){
        gpio_output(cols[i]);
        gpio_write(cols[i],1);   // default HIGH
    }

    // configure rows as inputs with pullups
    for(int i=0;i<4;i++){
        gpio_input_pullup(rows[i]);
    }
    
    while(1){
        for(int c=0;c<3;c++){
            gpio_write(cols[c], 0);  // pull column low
            for(int r=0;r<4;r++){
                if(gpio_read(rows[r]) == 0){
                    printf("Pressed: %c\n", keys[r][c]);
                    usleep(300000); // 300ms debounce
                }
            }
            gpio_write(cols[c], 1);  // back high
        }
        usleep(10000); // 10ms delay
    }
    return 0;
}