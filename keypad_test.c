#include <stdio.h>
#include <wiringPi.h>

int cols[3] = {5, 6, 13};      // C1 C2 C3
int rows[4] = {12, 16, 20, 21}; // R1 R2 R3 R4

char keys[4][3] = {
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','#'}
};

int main() {

    wiringPiSetupGpio();

    // setup columns
    for(int i=0;i<3;i++){
        pinMode(cols[i], OUTPUT);
        digitalWrite(cols[i], HIGH);
    }

    // setup rows
    for(int i=0;i<4;i++){
        pinMode(rows[i], INPUT);
        pullUpDnControl(rows[i], PUD_UP);
    }

    while(1){

        for(int c=0;c<3;c++){

            digitalWrite(cols[c], LOW);

            for(int r=0;r<4;r++){
                if(digitalRead(rows[r]) == LOW){
                    printf("Pressed: %c\n", keys[r][c]);
                    delay(300);
                }
            }

            digitalWrite(cols[c], HIGH);
        }

        delay(10);
    }

    return 0;
}