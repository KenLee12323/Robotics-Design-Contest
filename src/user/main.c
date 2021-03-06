
#include "main.h"
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

u32 ticks_img = 0;
u32 ticks_sec_img = 0;
u16 servo_pos = 750;
u8 speed = 20;
u16 speed_indic[3] = {RGB888TO565(0xC72929), RGB888TO565(0xFFC72C), RGB888TO565(0x40CA77)};
const int MAXBUFFER = 100;
int buffer_index = 0;

char global_led_on = 0;
char bool_need_clear_buffer = 1;
char bool_command_finish = 0;
char manual_mode = 0;

int timeSinceLastCommand;
int curTime;
int ccdTime = 0;
int ccd_rate = 50;
const int CCD_THRESH = 100;
const int WINDOWSIZE = 10;

u8 medianCCD[128] = {0};
u8 schmittCCD[128] = {0};
int sumDiffCCD[128] = {0};

char buffer[MAXBUFFER] = {0};
char manual_buffer[100];
char curKey = '\0';

const int RIGHTMOST = 930;
const int LEFTMOST = 1090;
const int CENTER = 1010;

void change_speed(void) {
    static u8 count = 0;
    count++;
    speed = 20; //case 0
    switch (count % 3) {
        case 2: //speed = 60
            speed += 20;
        case 1: //speed = 40
            speed += 20;
    }
    tft_fill_area(72, 72, 12, 12, speed_indic[count % 3]);
}

void use_motor(long value, long id) {
    if (value < 100 && value > 0) {
        motor_control((MOTOR_ID) id, 1, value);
    } else {
        uart_tx(COM3, "motor value is out of range\n");
    }
}

void use_servo(long value, long id) {
    if (value < 1050 && value > 450) {
        servo_control((SERVO_ID) id, value);
    } else {
        uart_tx(COM3, "servo value is out of range\n");
    }
}

void use_pneumatic(long value, long id) {
    pneumatic_control((PNEUMATIC_ID) id, value); //1 is on, others is off
}

void use_led(long value, long id) {
    if (value == 1) {
        led_on((LED_ID) id);
        uart_tx(COM3, "TURNED LED %ld ON\n", id);
    } else {
        led_off((LED_ID) id);
        uart_tx(COM3, "TURNED LED %ld OFF\n", id);
    }
}

void buffer_clear() {
    if (bool_need_clear_buffer) {
        bool_need_clear_buffer = 0;
        buffer_index = 0;
        uart_tx(COM3, "\nBuffer: ");
    }
}

void uart_listener_buffer(const u8 byte) {

}

void uart_listener(const u8 byte) {
    curTime = get_real_ticks();
    timeSinceLastCommand = 0;
    buffer[buffer_index++] = byte;
    buffer[buffer_index] = '\0';
    uart_tx(COM3, "%c", byte);
    if (byte == '.') {  //Mark end of command
        bool_command_finish = 1;
        buffer_index = 0;
    }
    if (byte == 'x') {  //If you make a typo, press x to reset buffer
        bool_need_clear_buffer = 1;
        manual_mode = 0;
        buffer_index = 0;
    }
}

float getMedian(const int a[]) {
    int arr[WINDOWSIZE] = {0};
    for (int k = 0; k < WINDOWSIZE; k++) { //copy into temporary array
        arr[k] = a[k];
    }
    int key, i, j;
    for (i = 2; i < WINDOWSIZE; i++) { //selection sort
        key = arr[i];
        j = i - 1;
        while (j > 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
    return (WINDOWSIZE % 2 == 1 ? arr[WINDOWSIZE / 2] : (arr[WINDOWSIZE / 2 - 1] + arr[WINDOWSIZE / 2]) / 2);
}

void runSchmitt() {
    int k;
    for (k = 0; k < 128; k++) {
        schmittCCD[k] = (medianCCD[k] < CCD_THRESH) ? 1 : 158;
    }
}

int abs(int a) {
    return a < 0 ? -a : a;
}

void calculateSumPrefix(int *leftCandidate, int *rightCandidate) {
    int k;
    for (k = 0; k < 128 - 1; k++) {
        sumDiffCCD[k] = schmittCCD[k] - schmittCCD[k + 1];
    }

    for (k = 0; k < 127; k++) { //negative value means close to left edge
        if (sumDiffCCD[k] < 0) {
            *leftCandidate = k;
            break;
        }
    }

    for (k = 126; k >= 0; k--) {
        if (sumDiffCCD[k] > 0) {
            *rightCandidate = k;
            break;
        }
    }
}

void drawLine(int val, int isHorizontal, u16 color) {
    int k;
    if (isHorizontal) {
        for (k = 0; k < 159; k++)
            tft_put_pixel(k, val, color);
    } else {
        for (k = 0; k < 159; k++) {
            tft_put_pixel(val, k, color);
        }
    }
}

void runMedianFilter() {
    int curWindow[WINDOWSIZE] = {0};
    int indexOfOldest = 0;
    //initialize the window
    for (int k = 0; k < WINDOWSIZE; k++) {
        curWindow[k] = linear_ccd_buffer1[k];
    }
    //int firstMaxMedianIndex = 0;
    //int lastMaxMedianIndex = 0;
    for (int j = 0; j <= WINDOWSIZE / 2; j++) {
        medianCCD[j] = getMedian(curWindow);
    }

    for (int k = WINDOWSIZE; k < 128; k++) {
        curWindow[indexOfOldest] = linear_ccd_buffer1[k];
        indexOfOldest++;
        indexOfOldest %= WINDOWSIZE;
        medianCCD[k - WINDOWSIZE / 2] = getMedian(curWindow);
        /*
        if (medianCCD[k-WINDOWSIZE/2] >= medianCCD[lastMaxMedianIndex]) {
        lastMaxMedianIndex = k - WINDOWSIZE/2;
        if (medianCCD[k-WINDOWSIZE/2] > medianCCD[firstMaxMedianIndex])
        firstMaxMedianIndex = k - WINDOWSIZE/2;
        }
        */
    }
    for (int k = 128 - WINDOWSIZE; k < 128; k++) {
        medianCCD[k] = getMedian(curWindow);
    }
}

void bluetooth_handler() {
    if (bool_command_finish) {
        bool_command_finish = 0;
        uart_tx(COM3, "\nCOMPLETE COMMAND: %s\n", buffer);

        char *cmdptr = strchr(buffer, ':'); //Locate ptr where the char : is first found
        char *valptr = cmdptr + 1;
        char *idptr = cmdptr - 1;
        int val = strtol(valptr, NULL, 10); //Obtain Value
        *cmdptr = '\0';
        int id = strtol(idptr, NULL, 10); //Obtain ID
        *idptr = '\0';
        uart_tx(COM3, "COMMAND: %s   ", buffer);
        uart_tx(COM3, "ID: %ld   ", id);
        uart_tx(COM3, "VAL: %ld\n", val);

        if (strstr(buffer, "led")) { //if detect substring led(strstr returns a pointer)
            bool_need_clear_buffer = 1;
            use_led(val, id); //LED
        } else if (strstr(buffer, "motor")) { //if detect substring motor
            use_motor(val, id); //MOTOR
            uart_tx(COM3, "motor %ld is on \n", id);
        } else if (strstr(buffer, "servo")) { //if detect substring servo
            use_servo(val, id); //SERVO
            uart_tx(COM3, "servo %ld is on \n", id);
        } else if (strstr(buffer, "pneumatic")) {
            use_pneumatic(val, id); //PNEUMATIC
            uart_tx(COM3, "pneumatic %ld is on \n", id);
        }
        bool_need_clear_buffer = 1;
    }
    if(strstr(buffer, "manual")) {
    	manual_mode = 1;
    	bool_need_clear_buffer = 1;
    }
    //TODO: Need to test on car
    if(manual_mode){
	    switch (buffer[0]) {
	    	case 'w':
	    		uart_tx(COM3, "w ");
	    	case 'a':
	    		uart_tx(COM3, "a ");
	    	case 's':
	    		uart_tx(COM3, "s ");
	    	case 'd':
	    		uart_tx(COM3, "d ");
	    	default:
	    		bool_need_clear_buffer = 1;
	    }
	}
    buffer_clear();
}

void init_all() {
    led_init();
    gpio_init();
    ticks_init();
    linear_ccd_init();
    adc_init();
    button_init();
    set_keydown_listener(BUTTON2, &change_speed);
    servo_init(143, 10000, 0);
    tft_init(2, BLACK, WHITE);
    uart_init(COM3, 115200);
    uart_interrupt_init(COM3, &uart_listener); //com port, function
    uart_tx(COM3, "initialize\nFormat -- CommandId:Val.");
    motor_init(143, 10000, 0);
}

int main() {
    init_all();

    int h;
    int dir;
    int leftEdge, rightEdge;
    int avg = 0;

    while (1) {
        //motor_control(0, 0, 50); //id, direction, magnitude
        if (read_button(BUTTON1) == 0 && servo_pos < LEFTMOST) {
            servo_pos += speed;
            tft_fill_area(46, 72, 25, 12, BLACK);
            tft_prints(46, 72, "%d", servo_pos);
            servo_control(SERVO1, servo_pos);
        }

        if (read_button(BUTTON3) == 0 && servo_pos > RIGHTMOST) {
            servo_pos -= speed;
            tft_fill_area(46, 72, 25, 12, BLACK);
            tft_prints(46, 72, "%d", servo_pos);
            servo_control(SERVO1, servo_pos);
        }
        //This is a comment
        if (get_real_ticks() - ccdTime >= ccd_rate) { //Update by CCD Rate
            ccdTime = get_real_ticks();
            int k;
            for (k = 0; k < 128; k++) { //Clear CCD Screen
                tft_put_pixel(k, 159 - linear_ccd_buffer1[k], BLACK);
                tft_put_pixel(k, 159 - medianCCD[k], BLACK);
                tft_put_pixel(k, 159 - schmittCCD[k], BLACK);
            }

            if (leftEdge != -1) {
                drawLine(leftEdge, 0, BLACK);
            }
            if (rightEdge != -1) {
                drawLine(rightEdge, 0, BLACK);
            }
            drawLine(avg, 0, BLACK);

            linear_ccd_read();
            runMedianFilter();
            runSchmitt();

            leftEdge = -1;
            rightEdge = 128;
            calculateSumPrefix(&leftEdge, &rightEdge);

            //tft_fill_area(50, 50, 60, 20, BLACK);
            if (leftEdge != -1) {
                drawLine(leftEdge, 0, GREEN);
            }
            if (rightEdge != 128) {
                drawLine(rightEdge, 0, GREEN);
            }

            avg = (leftEdge + rightEdge) / 2;
            drawLine(avg, 0, RED);

            tft_fill_area(50, 50, 50, 20, BLACK);

            if (avg < 64 - 10) {
                tft_prints(50, 50, "go left");
                servo_control(SERVO1, CENTER + (LEFTMOST - CENTER) * ((64 - avg) / 10.0));
            } else if (avg > 64 + 10) {
                tft_prints(50, 50, "go right");
                servo_control(SERVO1, CENTER - (CENTER - RIGHTMOST) * ((avg - 64) / 10.0));
            } else {
                servo_control(SERVO1, CENTER);
            }

            for (k = 0; k < 128; k++) { //Add CCD onto Screen
                tft_put_pixel(k, 159 - linear_ccd_buffer1[k], RED);
                tft_put_pixel(k, 159 - schmittCCD[k], GREEN);
                tft_put_pixel(k, 159 - medianCCD[k], WHITE);
            }
            for (h = 0; h < 159; h += 20) {
                tft_prints(2, h, "%d", 159 - h);
            }
        }
        bluetooth_handler();
    }
}
//UART listener
void uart3_listener(const u8 byte) {
    u8 ledno = byte - (u8) '1';
    if (ledno >= 3) return;
    led_toggle((LED_ID) ledno);
}
