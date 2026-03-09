#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int cols[3] = {5,6,13};
int rows[4] = {12,16,20,21};
char keys[4][3] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};

// Helper to set column high/low
void set_col(int c, int high) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "raspi-gpio set %d %s", c, high ? "dh" : "dl");
    system(cmd);
}

// Helper to read a row
int read_row(int r) {
    char cmd[64];
    char buf[64];
    snprintf(cmd, sizeof(cmd), "raspi-gpio get %d", r);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    fgets(buf, sizeof(buf), fp);
    pclose(fp);
    // buf looks like: "GPIO 12: level=1 fsel=0 func=INPUT pull=UP"
    return (strstr(buf, "level=0") != NULL) ? 0 : 1;
}

int main() {
    // setup done manually via raspi-gpio before running
    while(1){
        for(int c=0;c<3;c++){
            set_col(cols[c], 0);  // pull column low
            for(int r=0;r<4;r++){
                if(read_row(rows[r]) == 0){
                    printf("Pressed: %c\n", keys[r][c]);
                    usleep(300000); // 300ms debounce
                }
            }
            set_col(cols[c], 1);  // back high
        }
        usleep(10000); // 10ms delay
    }
    return 0;
}