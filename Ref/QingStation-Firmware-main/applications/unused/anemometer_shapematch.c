/*
 * Copyright (c) 2020-2021, Jianjia Ma
 * majianjia@live.com
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author           Notes
 * 2021-02-06     Jianjia Ma       the first version
 */
#include "stdio.h"
#include "stdlib.h"
#include "math.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "drv_anemometer.h"
#include "recorder.h"
#include "configuration.h"
#include "data_pool.h"

#define DBG_TAG "anemo"
//#define DBG_LVL LOG_LVL_ERROR
#define DBG_LVL LOG_LVL_DBG
#include <rtdbg.h>

enum{
    NORMAL = 0,
    ERR_MSE_NAN = 1,
    ERR_SHAPE_MISMATCH = 2,
    ERR_WINDSPEED = 3
} ERR_CODE;

// pulse generation/modulation, pulse are pwm with 0~99 = 0%~100%
#define H 99
#define L 0

// Double rate (80k), to control the +1 or −1 phase
// this is a bit tricky -- this is the only way to make it work.
// STM32's timer seems to require the first cycle not to be 100% width.
// So there is a dummy 'L' in each pulse, as well as a dummy 'L' in each end if the end is not L.
// A '+' is 'H, L', A '-' is 'L, H', phase of 40Khz
// M = modulation frequency
// B = barker code type

const uint16_t cpulse[] = {L, H, L, H, L, H, L, H, L, H, L, H, H, L, H, L, H, L, H, H, L, H, L, L, H}; // ++++++---++-+
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, H, L, H, H, L, H, L, H, L, H, H, L, H, L, L, H}; // +++++---++-+
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, H, L, H, H, L, H, L, H, L, H, H, L, H, L}; // +++++---++ // best for single peak method


//const uint16_t cpulse[] = {L, H, L,  H, L, L, H, L, H, H, L, H, L, H, L, H, L}; // 1011 M20
//const uint16_t cpulse[] = {L, H, L, H, L, L, H, L, H, L, H, L, H, L, L, H, L, H, L}; // ++----++// B2, M:10k
//const uint16_t cpulse[] = {L, H, L, H, L, L, H, L, H, L, H, L, H, L, L, H, L, H, L, H, L, H, L, L, H, L, H, L}; // ++--++----++ B3, M10k
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, H, L};                   // normal -> ++++
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, L, H, L, H, L};          // +++--
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, L, H, L, H, L, H, L};    // +++---
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, H, L, H, H, L, H, L, H, L, H, L}; // +++++---
//const uint16_t cpulse[] = {L, H, L, H, L, L, H, H, L};                   // ++-+ B4.1, M40k
//const uint16_t cpulse[] = {L, H, L, L, H, H, L, L, H, L, H, H, L, H, L, L, H, L}; // +-+--++- B4.1 M20k
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, L, H, L};                // +++- B4.2 M40k
//const uint16_t cpulse[] = {L, H, L, H, L, H, L, L, H, L, H,  H, L, L, H, L}; // barker-code 7 -> +++--+-
const uint32_t pulse_len = sizeof(cpulse) / sizeof(uint16_t);

// ADC = 1Msps, 500sample = 0.5ms ToF ~= 0.17m
// speed of sound: ~340m/s
// 500 sample  = 0.5ms ~= 0.17m
// 1000 sample = 1ms   ~= 0.34m
#define ADC_SAMPLE_LEN (1000)
uint16_t adc_buffer[4][ADC_SAMPLE_LEN] = {0};
float sig[ADC_SAMPLE_LEN] = {0};
float sig2[ADC_SAMPLE_LEN] = {0}; // buffer for filter
float sig_level[4] = {0};   // signal level for each channels

// where the process started -> to avoid the direct sound propagation at the beginning. Depended on the mechanical structure.
// Same unit of ADC sampling -> us.
#define DEADZONE_OFFSET ((int)(sizeof(cpulse) / sizeof(uint16_t) * 12.5) + 25) // AVOID overlapping the pulse
#define VALID_LEN  (ADC_SAMPLE_LEN - DEADZONE_OFFSET)
#define ZEROCROSS_LEN   (6)
#define NUM_ZC_AVG      (6) // number of zerocrossing to calculate the beam location. better to be even number

// to define the shape of to identify the echo beam
#define PEAK_LEFT   (8)
#define PEAK_MAIN   PEAK_LEFT
#define PEAK_RIGHT  (8)
#define PEAK_LEN    (PEAK_LEFT + PEAK_RIGHT + 1)
#define PEAK_ZC     (5)  // start from which peak to identify the zero crossing. (2=3rd)

#define IS_SIGN_DIFF(a, b) (!(signbit(a) == signbit(b))) // this cover 0, but have fixed width

// peak to peak mini distance in peak detection.
#define MINI_PEAK_DISTANCE  (5)

// filters
#define COEFF_40K_2K_BP_1ORDER {{0.0124111,1.0},{0.0,-1.9132751},{-0.0124111,0.9751779}}
#define COEFF_40K_2K_BP_2ORDER {{0.0001551,1.0},{0.0,-3.840213},{-0.0003103,5.6515555},{0.0,-3.7725641},{0.0001551,0.9650812}}
#define COEFF_40K_2K_BP_3ORDER {{1.9e-06,1.0},{0.0,-5.763269},{-5.8e-06,14.02188},{0.0,-18.4249013},{5.8e-06,13.7888872},{0.0,-5.5733324},{-1.9e-06,0.9509757}}

#define COEFF_40K_10K_BP_1ORDER {{0.0304687,1.0},{0.0,-1.8790705},{-0.0304687,0.9390625}}
#define COEFF_40K_10K_BP_2ORDER {{0.0009447,1.0},{0.0,-3.7901898},{-0.0018894,5.504279},{0.0,-3.6254026},{0.0009447,0.9149758}}
#define COEFF_40K_10K_BP_3ORDER {{2.91e-05,1.0},{0.0,-5.6926121},{-8.74e-05,13.6786558},{0.0,-17.7500413},{8.74e-05,13.1173539},{0.0,-5.2350269},{-2.91e-05,0.8818931}}

const float bp_coeff[][2] = COEFF_40K_10K_BP_1ORDER;
const int   bp_coeff_order = sizeof(bp_coeff)/2/sizeof(float)/2;


// return offset
uint32_t match_filter(float* signal, uint32_t signal_len, float* pattern, uint32_t pattern_len, float* output)
{
   float max = 0;
   float sum = 0;
   uint32_t idx = 0;
   for(uint32_t i=0; i<signal_len-pattern_len; i++)
   {
       for(uint32_t j=0; j<pattern_len; j++)
           sum += pattern[j] * signal[i+j];
       if(output)
           output[i] = sum;
       if(sum > max)
       {
           max = sum;
           idx = i;
       }
       sum = 0;
   }
   return idx;
}

float maxf(float* sig, int len)
{
    float max = sig[0];
    for(int i=1; i<len; i++)
        max = MAX(sig[i], max);
    return max;
}

// find the max index
int32_t argmaxf(float* sig, int32_t len)
{
    float max = sig[0];
    uint32_t arg = 0;

    for(uint32_t i=0; i<len; i++)
    {
        if(sig[i] > max)
        {
            arg = i;
            max = sig[i];
        }
    }
    return arg;
}

float minf(float* sig, int len)
{
    float min = sig[0];
    for(int i=1; i<len; i++)
        min = MIN(sig[i], min);
    return min;
}

int arg_minf(float* sig, int len)
{
    float min = sig[0];
    int arg = 0;
    for(uint32_t i=0; i<len; i++){
        if(sig[i] < min){
            arg = i;
            min = sig[i];
        }
    }
    return arg;
}

// to normalize the signal to -1 ~1
void normalize(float* pattern, uint32_t len)
{
    float max = 0;
    for(int i=0; i<len; i++)
        max = MAX(abs(pattern[i]), max);
    for(int i=0; i<len; i++)
        pattern[i] = pattern[i] / max;
}


// insert sort
static void sort(float arr[], uint32_t len)
{
    uint32_t i,j;
    for (i=1; i<len;i++)
    {
        float temp = arr[i];
        for (j=i; j>0 && arr[j-1]>temp; j--)
            arr[j] = arr[j-1];
        arr[j] = temp;
    }
}

// find the exact interpolation
// output the offset of the signal in sub-digit resolution.
int linear_interpolation_zerocrossing(float* sig, uint32_t sig_len, float* out, uint32_t num_zero_cross)
{
    #define IS_SIGN_DIFF_NO_ZERO(a, b) (a*b<0)
    uint32_t cross = 0;
    for(uint32_t i=0; i<sig_len-1 && cross<num_zero_cross; i++)
    {
        if(sig[i] == 0) // in case there is a zero.
        {
            out[cross] = i;
            cross++;
        }
        else if(IS_SIGN_DIFF_NO_ZERO(sig[i], sig[i+1]))
        {
            // do interpolation using y=ax+b
            float a = sig[i+1] - sig[i];    // a=(y2 - y1)/(x2 - x1) while x2 - x1 = 1
            float b = sig[i];               // b=y1
            float x = -b/a;                 // x=(y-b)/a zero crossing.
            out[cross] = x + i;
            // cross
            cross++;
        }
    }
    return  cross;
}

// input the calibration signal (sampled when no signal)
float get_zero_level(uint16_t* raw, uint32_t len)
{
    float sum = 0;
    for(uint32_t i=0; i<len; i++)
        sum += raw[i];
    return sum/len;
}

int find_next_turning(float *sig, int len)
{
    float pre_dt = sig[3]-sig[2]; // jump the first point for better stability
    float dt = 0;
    for(int i=3; i<len-1; i++)
    {
        dt = sig[i+1] - sig[i];
        if(IS_SIGN_DIFF(pre_dt, dt))
            return i;
        pre_dt = dt;
    }
    return 0;
}

int find_prev_turning(float *sig, int len)
{
    float *p = sig;
    float pre_dt = *(p-2) - *(p-3);
    float dt = 0;
    for(int i=3; i<len-1; i++)
    {
        dt = *(p-i) - *(p-i-1);
        if(IS_SIGN_DIFF(pre_dt, dt))
            return -i;
        pre_dt = dt;
    }
    return 0;
}


int capture_peaks_from(float* sig, int sig_len, float peaks[][2], int peak_len, float threshold)
{
    int peak_detected_len = 0;
    int max_idx = argmaxf(sig, sig_len); // this is the middle peak (main).
    int prev_peak;
    int max_distance_right = 25 * (peak_len +2);
    threshold = sig[max_idx] * threshold;

    int sig_idx =0;
    for(int i=0; i<peak_len; i++)
    {
       int turning_idx = find_next_turning(&sig[sig_idx], sig_len - sig_idx);
       if(turning_idx == 0)
           break;
       sig_idx += turning_idx;
       if(sig_idx > sig_len || sig_idx - max_idx > max_distance_right) // max searching distance
           break;

       if(fabs(sig[sig_idx]) >= threshold && fabs(prev_peak - sig_idx) >= MINI_PEAK_DISTANCE)
       {
           peaks[i][0] = sig_idx;
           peaks[i][1] = sig[sig_idx];
           peak_detected_len ++;
           prev_peak = sig_idx;
       }
    }

    return peak_detected_len;
}

// capture a few peak moment around the peak points
// buffer size should equal to (peak_left_len + 1 + peak_right_len)
// buffer is stored in peaks[][0] = index, peaks[][1] = value.
int capture_peaks(float* sig, int sig_len, float peaks[][2], int peak_left_len, int peak_right_len, float threshold)
{
    int peak_detected_len = 0;
    int max_idx = argmaxf(sig, sig_len); // this is the middle peak (main).
    int peak_idx;
    int prev_peak;
    int max_distance_left = 25 * (peak_left_len +2);
    int max_distance_right = 25 * (peak_right_len +2);
    threshold = sig[max_idx] * threshold;

    // main peak
    peak_idx = peak_left_len;
    peaks[peak_idx][0] = max_idx;
    peaks[peak_idx][1] = sig[max_idx];
    peak_detected_len ++;

    // scan for the peak after max. (right)
    int sig_idx = max_idx;          // signal start form the right of main peak.
    peak_idx = peak_left_len + 1;  // start from the right of main peak.
    prev_peak = 0;
    for(int i=0; i<peak_right_len; i++)
    {
       int turning_idx = find_next_turning(&sig[sig_idx], sig_len - sig_idx);
       if(turning_idx == 0)
           break;
       sig_idx += turning_idx;
       if(sig_idx > sig_len || sig_idx - max_idx > max_distance_right) // max searching distance
           break;

       if(fabs(sig[sig_idx]) >= threshold && fabs(prev_peak - sig_idx) >= MINI_PEAK_DISTANCE)
       {
           peaks[peak_idx][0] = sig_idx;
           peaks[peak_idx][1] = sig[sig_idx];
           peak_idx ++;
           peak_detected_len ++;
           prev_peak = sig_idx;
       }
    }

    // scan for the peak before max. (left)
    sig_idx = max_idx;          // signal start form the left of main peak.
    peak_idx = peak_left_len - 1;  // start from the left of main peak.
    for(int i=peak_idx; i>=0 && peak_idx>=0; i--)
    {
       int turning_idx = find_prev_turning(&sig[sig_idx], sig_idx);
       if(turning_idx == 0)
           break;
       sig_idx += turning_idx;
       if(max_idx - sig_idx > max_distance_left) // maximum searching distance
           break;

       if(fabs(sig[sig_idx]) >= threshold && fabs(prev_peak - sig_idx) >= MINI_PEAK_DISTANCE)
       {
           peaks[peak_idx][0] = sig_idx;
           peaks[peak_idx][1] = sig[sig_idx];
           peak_idx --;
           peak_detected_len ++;
           prev_peak = peak_idx;
       }
    }
    return peak_detected_len;
}


int locate_main_peak(float peaks[][2], int peak_len)
{
    // locate the main peak by the interception of 2 linear functions
    // the first function is from the peaks before the maximum peaks,
    // the second function is from the peaks after.
    float a1, b1, a2, b2;
    float p[6][2]; // points (x,y) y=ax+b
    p[0][0] = peaks[0][0];
    p[0][1] = peaks[0][1];
    p[1][0] = peaks[2][0];
    p[1][1] = peaks[2][1];
    p[2][0] = peaks[4][0];
    p[2][1] = peaks[4][1];

    p[3][0] = peaks[peak_len-5][0];
    p[3][1] = peaks[peak_len-5][1];
    p[4][0] = peaks[peak_len-3][0];
    p[4][1] = peaks[peak_len-3][1];
    p[5][0] = peaks[peak_len-1][0];
    p[5][1] = peaks[peak_len-1][1];

    float avg_x = 0;
    float avg_y = 0;
    float n = 0;
    float m = 0;
    for(int i=0; i<3; i++) // average x
        avg_x += p[i][0];
    avg_x /= 3;

    for(int i=0; i<3; i++) // average y
        avg_y += p[i][1];
    avg_y /= 3;

    for(int i=0; i<3; i++){
        n += (p[i][0] - avg_x)*(p[i][1] - avg_y);
        m += (p[i][0] - avg_x)*(p[i][0] - avg_x);
    }
    a1 = n/m;
    b1 = avg_y - avg_x *a1;

    avg_x = 0; avg_y = 0; n = 0; m = 0;
    for(int i=3; i<6; i++) // average x
        avg_x += p[i][0];
    avg_x /= 3;
    for(int i=3; i<6; i++) // average y
        avg_y += p[i][1];
    avg_y /= 3;
    for(int i=3; i<6; i++){
        n += (p[i][0] - avg_x)*(p[i][1] - avg_y);
        m += (p[i][0] - avg_x)*(p[i][0] - avg_x);
    }
    a2 = n/m;
    b2 = avg_y - avg_x *a2;

    float x = (b2-b1)/(a1-a2);
    float y = x * a1 + b1;  // not needed.

    // search for main peak
    int idx;
    for(idx=0; idx<peak_len; idx++)
    {
        if(peaks[idx][1] > 0){
            if(peaks[idx][0] > x)
                break;
        }
    }
    return idx; // return the offset of the main peak
}

// compare 2 peaks arrays, find offset of it.
int match_shape(float peaks1[][2], float peaks2[][2], int len, float mse[], int search_range)
{
    memset(mse, 0, sizeof(float)*search_range);
    for(int off = -search_range/2; off<=search_range/2; off++)
    {
        float sum = 0;
        float count = 0;
        int start_idx = -off;
        int stop_idx = len + off;
        if(start_idx < 0) start_idx = 0;
        if(stop_idx > len) stop_idx = len - off;
        for(int i=start_idx; i<stop_idx; i++)
        {
            if(peaks1[i][0] !=0 && peaks2[i][0] != 0) //
            {
                float v = peaks1[i][1] - peaks2[i+off][1];
                sum += v*v;
                count++;
            }
        }
        mse[off+search_range/2] = sum/count;
    }
    return arg_minf(mse, search_range);
}

// int16 -> float, also move data to zero_level. see if we need to scale it?
int preprocess(uint16_t *raw, float* out, float zero_level, uint32_t len)
{
    for(uint32_t i=0; i<len; i++){
        out[i] = ((float)raw[i] - zero_level);
    }
    return len;
}
// this version use the current signal's zero_level. no calibration needed.
float preprocess2(uint16_t *raw, float* out, uint32_t len)
{
    float zero_level = 0;
    for(uint32_t i=0; i<len; i++){
        zero_level += raw[i];
    }
    zero_level /= len;

    for(uint32_t i=0; i<len; i++){
        out[i] = ((float)raw[i] - zero_level);
    }
    return zero_level;
}

/*
// add a small LPF to avoid equal number, which cause turning point detection fail
void small_lpf(float* sig, uint32_t len)
{
    float momentum = sig[0];
    for(uint32_t i=1; i<len; i++){
        sig[i] = sig[i]*0.9 + momentum*0.1;
        momentum = sig[i];
    }
}*/

// this is a moving sum version, windows size = half wave of 40k
void small_lpf(float* sig, uint32_t len)
{
    #define WIN_SIZE 12
    float sum =0;
    for(uint32_t i=1; i<len-WIN_SIZE; i++){
        for(int j=0; j<WIN_SIZE; j++)
            sum+= sig[i+j];
        sig[i] = sum;
        sum = 0;
    }
}

// a generic fileters using pre-calculated b-a coefficient.
// y[i] = b[0] * x[i] + b[1] * x[i - 1] + b[2] * x[i - 2] - a[1] * y[i - 1] - a[2] * y[i - 2]...
void filter(float* x, float* y, uint32_t signal_len, const float ba[][2], const uint32_t orders)
{
    memset(y, 0, sizeof(float)*(orders*2+1)); // we dont use the first few data.
    for(int i=(orders*2+1); i<signal_len; i++)
    {
       y[i] = 0;
       for(int c=0; c < (orders*2+1); c++)
           y[i] += ba[c][0] + x[i-c] - ba[c][1] * y[i-c];
    }
}


void test_channel(uint32_t ch)
{
#define PWM_PIN GET_PIN(A, 6)
    rt_pin_mode(PWM_PIN, PIN_MODE_OUTPUT);
    set_output_channel(ch);
    rt_pin_write(PWM_PIN, 1);
    rt_pin_write(PWM_PIN, 0);
}

void test_print_raw()
{
    for(int j=0; j<ADC_SAMPLE_LEN; j++)
        rt_kprintf("%d\n", adc_buffer[NORTH][j]);
    for(int j=0; j<ADC_SAMPLE_LEN; j++)
        rt_kprintf("%d\n", adc_buffer[SOUTH][j]);
    for(int j=0; j<ADC_SAMPLE_LEN; j++)
        rt_kprintf("%d\n", adc_buffer[EAST][j]);
    for(int j=0; j<ADC_SAMPLE_LEN; j++)
        rt_kprintf("%d\n", adc_buffer[WEST][j]);
}

// ref http://www.sengpielaudio.com/calculator-airpressure.htm
// Speed of sound c ≈ 331.3 + 0.6 ϑ  (m/s)
// Temperature ϑ ≈ (331.3 − c) / 0.6
float speed_of_sound_from_T(float temperature){
    return 20.05f*sqrtf(temperature + 273.15); // more accurate.
    //return 331.3+0.6*temperature;
}

float average(float sig[], int num)
{
    float sum=0;
    for(int i=0; i<num; i++)
        sum += sig[i];
    return sum/num;
}

// test only
// send raw ADC data to Processing script to visualize the data.
void send_to_processing(int cycle, int start_idx, int stop_idx, const uint16_t* pulse, const uint16_t pulse_len)
{
    for (int i = 0; i<cycle; i++)
    {
        for(int idx = 0; idx < 4; idx ++){
            ane_measure_ch(idx,  pulse, pulse_len, adc_buffer[idx], ADC_SAMPLE_LEN, true);
        }
        for(int j=start_idx; j<stop_idx; j++){
            printf("%d,%d,%d,%d\n", adc_buffer[0][j],adc_buffer[1][j],adc_buffer[2][j],adc_buffer[3][j]);
        }
        printf("reset\n");
        rt_thread_delay(50);
    }
}

int zero_offset_update(int times, float* zero_level, int offset)
{
    // measure zero_level - calibration.
    for (int i = 0; i<times; i++){
        for(uint32_t idx=0; idx < 4; idx++){
            adc_sample((ULTRASONIC_CHANNEL)idx, adc_buffer[idx], ADC_SAMPLE_LEN);
            zero_level[idx] += get_zero_level(&adc_buffer[idx][offset], ADC_SAMPLE_LEN - offset);
        }
    }
    for(int idx = 0; idx < 4; idx ++)
        zero_level[idx] /= times;
    return 0;
}

// record the adc to file.
int record_raw(const char* path, int times, bool is_sample, uint16_t* pulse, uint16_t pulse_len)
{
    recorder_t *recorder = NULL;
    char str[32] = {0}; // buffer is small, taker care.
    recorder = recorder_create(path, "North,South,East,West", 20000);
    for (int i = 0; i<times; i++)
    {
        if(is_sample)
            for(int idx = 0; idx < 4; idx ++)
                ane_measure_ch(idx,  pulse, pulse_len, adc_buffer[idx], ADC_SAMPLE_LEN, false);

        for(int j=0; j<ADC_SAMPLE_LEN; j++){
            sprintf(str, "%d,%d,%d,%d\n", adc_buffer[0][j], adc_buffer[2][j],adc_buffer[1][j],adc_buffer[3][j]);
            while(recorder_write(recorder, str) != RT_EOK)
                rt_thread_delay(1);
        }
    }
    recorder_delete(recorder);
    return 0;
}

// print raw to terminal
void print_raw(int times, bool is_sample, uint16_t* pulse, uint16_t pulse_len)
{
    for (int i = 0; i<times; i++)
    {
        if(is_sample)
            for(int idx = 0; idx < 4; idx ++)
                ane_measure_ch(idx,  pulse, pulse_len, adc_buffer[idx], ADC_SAMPLE_LEN, false);
        test_print_raw();
    }
}

// return the channel that connected.
// each bit represent the channel.
// 0 = no connection.
uint32_t check_transducer_data(float zero[])
{
   uint32_t ch = 0;
   int max = 0;
   for(int i=0; i<4; i++)
   {
       for(int j=DEADZONE_OFFSET; j<ADC_SAMPLE_LEN; j++)
           max = MAX(max, adc_buffer[i][j]);
       if(max > zero[i] + 50)
           ch = ch | (0x01 << i);
   }
   return ch;
}

uint32_t check_transducer_connection(const uint16_t* pulse, const uint32_t pulse_len)
{
   float zero[4];
   for(int i=0; i<4; i++){
       zero[i] = ane_measure_ch(i,  pulse, pulse_len, adc_buffer[i], ADC_SAMPLE_LEN, true);
   }
   return check_transducer_data(zero);
}

void get_pulse_offset(float offset[], float zero_cross[][ZEROCROSS_LEN], float propagation_time)
{
    // the offset between the first valid crossing to the wave that actually start.
    offset[NORTH] = propagation_time - average(zero_cross[NORTH], NUM_ZC_AVG);
    offset[SOUTH] = propagation_time - average(zero_cross[SOUTH], NUM_ZC_AVG);
    offset[EAST] = propagation_time - average(zero_cross[EAST], NUM_ZC_AVG);
    offset[WEST] = propagation_time - average(zero_cross[WEST], NUM_ZC_AVG);
}

// this update a normal peak values at a rate (represent a portion of new peaks)
void update_shape(float ref[PEAK_LEN][2], float curr[PEAK_LEN][2], float rate)
{
    for(int i=0; i<PEAK_LEN; i++)
        ref[i][0] = ref[i][0] * (1-rate) + curr[i][0]*rate;
}

void correlation(float* sig1, int len1, float* sig2, int len2, float* out)
{
    int len = len1 + len2;
    for (int i = 0; i < len; i++)
    {
        int32_t start2 = MAX(0, (len2-i)); // bracket needed.
        int32_t end2 = MIN(len2, (len2 - (i-len1)));
        int32_t start1 = MAX(0, (i - len2));
        float sum = 0;
        for (int n = start2; n < end2; n++)
            sum += sig1[start1++] * sig2[n];
        out[i] = sum;
    }
}

int dump_error_measurement(int error_count)
{
    recorder_t *recorder = NULL;
    time_t timep;
    char str[64] = {0}; // buffer is small, taker care.
    char filepath[64];

    if(access("/wind_err", 0)){
        mkdir("/wind_err", 777);
    }
    if(access("/wind_err", 0))
        return -1;

    time(&timep);
    strftime(str, 64, "%Y%m%d_%H%M%S", gmtime(&timep));
    snprintf(filepath, 64, "/wind_err/%s_%d_%s", str, error_count,"err.csv");
    recorder = recorder_create(filepath, "North,South,East,West",  20000);
    // dump
    for(int j=0; j<ADC_SAMPLE_LEN; j++){
        sprintf(str, "%d,%d,%d,%d\n", adc_buffer[0][j], adc_buffer[2][j],adc_buffer[1][j],adc_buffer[3][j]);
        while(recorder_write(recorder, str) != RT_EOK)
            rt_thread_delay(1);
    }
    recorder_delete(recorder);
    rt_thread_delay(20);
    return 0;
}

// output:
// -> zero_crossing
// -> calibrated shapes (peaks)
int calibration2(float* static_zero_cross, float* echo_shape, float* sig,
        const uint16_t *pulse, const uint16_t pulse_len)
{
    float sig_level[4];
    memset(static_zero_cross, 0, sizeof(float) * ZEROCROSS_LEN * 4);
    memset(echo_shape, 0, sizeof(float) * PEAK_LEN* 2 *4);

    // make a good masurement.
    for (int i = 0; i<16; i++){
        sig_level[NORTH] = ane_measure_ch(NORTH,  cpulse, pulse_len, adc_buffer[NORTH], ADC_SAMPLE_LEN, true);
        sig_level[SOUTH] = ane_measure_ch(SOUTH,  cpulse, pulse_len, adc_buffer[SOUTH], ADC_SAMPLE_LEN, true);
        sig_level[EAST] = ane_measure_ch(EAST,  cpulse, pulse_len, adc_buffer[EAST], ADC_SAMPLE_LEN, true);
        sig_level[WEST] = ane_measure_ch(WEST,  cpulse, pulse_len, adc_buffer[WEST], ADC_SAMPLE_LEN, true);
        if(fabs(sig_level[NORTH]-sig_level[SOUTH]) < 2 && fabs(sig_level[EAST]-sig_level[WEST]) < 2)
            break;
    }
    // find peaks and select a good channel as template.
    float distance[4];
    float peaks_zero[4][PEAK_LEN][2] = {0}; // collect 4 more.
    for(int idx=0; idx<4; idx++)
    {
        preprocess(adc_buffer[idx], sig2, sig_level[idx], ADC_SAMPLE_LEN);
        filter(sig2, sig, ADC_SAMPLE_LEN, bp_coeff, bp_coeff_order);
        normalize(&sig[DEADZONE_OFFSET], VALID_LEN);
        capture_peaks(&sig[DEADZONE_OFFSET], VALID_LEN, peaks_zero[idx], PEAK_LEFT, PEAK_RIGHT, 0.2);
        // find the maximum distance between the main and its near 2 peaks.
        distance[idx] = (peaks_zero[idx][PEAK_MAIN][1] - peaks_zero[idx][PEAK_MAIN - 2][1]) +
                (peaks_zero[idx][PEAK_MAIN][1] - peaks_zero[idx][PEAK_MAIN + 2][1]);
    }
    // select the channel which has max distance to the side peaks.
    int selected_ch;
    selected_ch = argmaxf(distance, 4);
    LOG_I("Shape offset based on channel: %s, peak distance %f, %f, %f, %f",
            ane_ch_names[selected_ch], distance[NORTH], distance[SOUTH], distance[EAST], distance[WEST]);

    // when we have the peaks, we can now use it as a template to capture all others channels.
    // add some small offset in case of little misalignment between channels.
    int start_idx = peaks_zero[selected_ch][0][0] - 8;

    // capture pattern.
    int count = 0;
    for (int i = 0; i<256 && count<32; i++)
    {
        sig_level[NORTH] = ane_measure_ch(NORTH,  cpulse, pulse_len, adc_buffer[NORTH], ADC_SAMPLE_LEN, true);
        sig_level[SOUTH] = ane_measure_ch(SOUTH,  cpulse, pulse_len, adc_buffer[SOUTH], ADC_SAMPLE_LEN, true);
        sig_level[EAST] = ane_measure_ch(EAST,  cpulse, pulse_len, adc_buffer[EAST], ADC_SAMPLE_LEN, true);
        sig_level[WEST] = ane_measure_ch(WEST,  cpulse, pulse_len, adc_buffer[WEST], ADC_SAMPLE_LEN, true);

        float zero_cross[4][ZEROCROSS_LEN] = {0};
        for(int idx = 0; idx < 4; idx++)
        {
            // convert to float
            preprocess(adc_buffer[idx], sig2, sig_level[idx], ADC_SAMPLE_LEN);
            // band pass filter.
            filter(sig2, sig, ADC_SAMPLE_LEN, bp_coeff, bp_coeff_order);
            // normalize to -1 to 1
            normalize(&sig[DEADZONE_OFFSET], VALID_LEN);
            // search original peaks, use to rough estimate the data.
            capture_peaks_from(&sig[DEADZONE_OFFSET + start_idx],  VALID_LEN-start_idx, peaks_zero[idx], PEAK_LEN, 0.2);
            //capture_peaks(&sig[DEADZONE_OFFSET + start_idx], VALID_LEN, peaks_zero[idx], 0, PEAK_RIGHT+PEAK_LEFT, 0.2);
            // recover timestamp
            for(int j=0; j<PEAK_LEN; j++)
                peaks_zero[idx][j][0] += DEADZONE_OFFSET + start_idx;
            // measure zero cross
            int off = peaks_zero[idx][PEAK_ZC][0];
            linear_interpolation_zerocrossing(&sig[off], ADC_SAMPLE_LEN-off, zero_cross[idx], ZEROCROSS_LEN);
            // recover the actual timestamp from start
            for(int j=0; j<ZEROCROSS_LEN; j++)
                zero_cross[idx][j] += off;
        }

        // record if the numbers looks correct
        if(fabs(zero_cross[NORTH][PEAK_ZC] - zero_cross[SOUTH][PEAK_ZC]) < 2 && // same channel
           fabs(zero_cross[WEST][PEAK_ZC] - zero_cross[EAST][PEAK_ZC]) < 2 &&
           fabs(zero_cross[NORTH][PEAK_ZC] - zero_cross[EAST][PEAK_ZC]) < 10 &&  // cross channel
           fabs(zero_cross[SOUTH][PEAK_ZC] - zero_cross[WEST][PEAK_ZC]) < 10)
        {
            count++;
            // sum them up
            for(int idx = 0; idx < 4; idx++)
            for(int j=0; j<ZEROCROSS_LEN; j++)
                *(static_zero_cross + idx*ZEROCROSS_LEN + j) += zero_cross[idx][j];

            // index average
            for(int idx = 0; idx < 4; idx++)
            for(int j=0; j<PEAK_LEN; j++)
            {
                *(echo_shape + (idx*PEAK_LEN + j)*2 + 0) += peaks_zero[idx][j][0];
                *(echo_shape + (idx*PEAK_LEN + j)*2 + 1) += peaks_zero[idx][j][1];
            }
        }
    }
    if(count == 0)
        return 0;
    // generate static zero cross.
    for(int idx = 0; idx < 4; idx ++)
        for(int j=0; j<ZEROCROSS_LEN; j++)
            *(static_zero_cross + idx*ZEROCROSS_LEN + j) /= count;

    for(int idx = 0; idx < 4; idx ++)
        for(int j=0; j<PEAK_LEN; j++){
            *(echo_shape + (idx*PEAK_LEN + j)*2 + 0) /= count;
            *(echo_shape + (idx*PEAK_LEN + j)*2 + 1) /= count;
        }
    return count;
}


//borrow from rain.c
typedef struct _ringbuff_t {
    float *buf;
    int32_t idx;
    int32_t size;
} ringbuffer_t;

static float ringbuffer_add(ringbuffer_t* data, float new)
{
    data->buf[data->idx] = new;
    data->idx ++;
    if(data->idx >= data->size)
        data->idx = 0;
    return 0;
}

static float ringbuffer_average(ringbuffer_t *data)
{
    float avg = 0;
    for(int i=0; i<data->size; i++)
        avg += data->buf[i];
    avg /= data->size;
    return avg;
}

static float ringbuffer_max(ringbuffer_t *data)
{
    float max = data->buf[0];
    for(int i=1; i<data->size; i++)
        max = MAX(max, data->buf[i]);
    return max;
}



bool is_ane_log = false;
void anemometer_info(int argc, void*argv){
    is_ane_log = !is_ane_log;
}
MSH_CMD_EXPORT(anemometer_info, print anemometer debugging information)

bool is_ane_proc = false;
void anemometer_processing(int argc, void*argv){
    is_ane_proc = !is_ane_proc;
}
MSH_CMD_EXPORT(anemometer_processing, send raw ADC to processing script.)

void thread_anemometer(void* parameters)
{
    //recorder_t *recorder = NULL;
    bool is_lightning_calibrating();
    char str_buf[128] = {0};
    sensor_config_t * cfg;
    anemometer_config_t * ane_cfg;

    rt_thread_delay(3000);

    // waiting for configuration load
    do{
        cfg = get_sensor_config_wait("Anemometer");
    }while(!cfg);
    ane_cfg = cfg->user_data;

    // parameters.
    // D=distance to reflector; alpha=angle of reflection.
    // wind speed: v = d/sin(a)*cos(a)((1/T_forward) - (1/T_backward))
    // sound speed: c = d/sin(a)*((1/T_forward) + (1/T_backward)
    // #define HEIGHT   0.05   // the height to the reflection panel.
    // #define PITCH    0.04   // the distance between 2 transceivers.
    float height = ane_cfg->height;
    float pitch = ane_cfg->pitch;
    float alpha = atanf(2*height/pitch);
    float cos_a = cosf(alpha);
    float sin_a = sinf(alpha);
    LOG_I("Height %dmm, Pitch:%dmm, ADC Dead Zone offset %d, ADC len %d",
            (int)(height*1000), (int)(pitch*1000), DEADZONE_OFFSET, VALID_LEN);

    // hardware power_on
    ane_pwr_control(80*1000, true);

    // wait for lightning sensor to calibrate. it causes huge noise to west channel (east)
    while(is_lightning_calibrating()) rt_thread_delay(100);

//    //test
//    if(is_ane_proc)
//        send_to_processing(1000, 0, ADC_SAMPLE_LEN-50, cpulse, pulse_len);

    // check connection
    LOG_I("Checking transducers connection.");
    if (!check_transducer_connection(cpulse, pulse_len))
    {
        LOG_W("No transducers connected.");
        while(check_transducer_connection(cpulse, pulse_len) != 0xf) // wait for all channel
            rt_thread_delay(1000);
        LOG_I("transducers connected.");
    }

    // cap charge
    for(uint32_t i=0; i<50; i++){
        ane_measure_ch(NORTH,  cpulse, pulse_len, adc_buffer[NORTH], ADC_SAMPLE_LEN, false);
        ane_measure_ch(SOUTH,  cpulse, pulse_len, adc_buffer[SOUTH], ADC_SAMPLE_LEN, false);
        ane_measure_ch(EAST,  cpulse, pulse_len, adc_buffer[EAST], ADC_SAMPLE_LEN, false);
        ane_measure_ch(WEST,  cpulse, pulse_len, adc_buffer[WEST], ADC_SAMPLE_LEN, false);
    }

    // collect zerocross base line for each channel.
    float static_zero_cross[4][ZEROCROSS_LEN] = {0};
    float ref_shape[4][PEAK_LEN][2] = {0};       // store the shape of the echo

    LOG_I("Calibrating anemometer, please place in calm wind.");
    int count = calibration2((float*)static_zero_cross, (float*)ref_shape, sig, cpulse, pulse_len);
    if(count < 5 && count !=0)
        LOG_W("Anemometer calibration is not good, based on %d measurements", count);
    else if (count == 0) {
        LOG_E("Anemometer calibration failed, release the constrains or select different pulse.");
    }else {
        LOG_I("Anemometer calibration completed, based on %d measurements", count);
    }
    rt_thread_mdelay(50);

    // Test zone
    //record_raw("/raw.csv", 100, true, pulse, pulse_len);
    //print_raw(1, true, pulse, pulse_len);

    // calculate the offset between the zerocrossings to the start of the wave
    float est_c = speed_of_sound_from_T(air_info.temperature);
    LOG_I("temp: %.1f degC, est_wind_speed: %.1fm/s", air_info.temperature, est_c);
    // the offset between the first valid crossing to the wave that actually start.
    float T = 2* height / (sin_a * est_c) * 1000000;
    float pulse_offset[4];

    get_pulse_offset(pulse_offset, static_zero_cross, T);
    memcpy(ane_cfg->pulse_offset, pulse_offset, 4 * sizeof(float)); // copy to system cfg
    save_system_cfg_to_file();
    /*
    // load calibration from file. implement later.
    if(ane_cfg->pulse_offset[NORTH] == 0 || ane_cfg->pulse_offset[EAST] == 0)
    {
        get_pulse_offset(pulse_offset, static_zero_cross, T);
        memcpy(ane_cfg->pulse_offset, pulse_offset, 4 * sizeof(float)); // copy to system cfg
        save_system_cfg_to_file();
    }
    else{
        memcpy(pulse_offset, ane_cfg->pulse_offset, 4 * sizeof(float));
        LOG_I("Loading channel offset from configuration file");
    }
    */

    LOG_I("Propagation time:%.2f, est offset: %.2f, %.2f, %.2f, %.2f",
            T, pulse_offset[0], pulse_offset[1],pulse_offset[2],pulse_offset[3]);

    // wind history buffer
    ringbuffer_t wind_hist;
    wind_hist.idx = 0;
    wind_hist.size = cfg->data_period *30 / 1000;
    wind_hist.buf = malloc(sizeof(float) * wind_hist.size); // data for 30s
    memset(wind_hist.buf, 0, sizeof(float) * wind_hist.size);

    // main body
    rt_tick_t period = cfg->data_period / cfg->oversampling;
    uint64_t err_count = 0;
    int oversampling_count = 0;
    float ns_v=0, ew_v=0; // wind speed
    float ns_c=0, ew_c=0; // sound speed -> these measurements should be very close, other wise the measurment is wrong.
    float v=0, c=0, c_acc=0;
    float ns_v_acc=0, ew_v_acc=0; // accumulation for oversampling
    float course=0;
    float mse_history[4] = {0}; // matching abnormal checking
    float c_history = 0; // sound speed for abnormal checking
    rt_tick_t last_dump = rt_tick_get(); // to limit the dumpping per second
    int err = NORMAL;
    while(1)
    {
        rt_thread_mdelay(20);
        if(!cfg->is_enable)
            continue;

        // make a new start
        err = NORMAL;

        // make a sample
        sig_level[NORTH] = ane_measure_ch(NORTH,  cpulse, pulse_len, adc_buffer[NORTH], ADC_SAMPLE_LEN, true);
        sig_level[SOUTH] = ane_measure_ch(SOUTH,  cpulse, pulse_len, adc_buffer[SOUTH], ADC_SAMPLE_LEN, true);
        sig_level[EAST] = ane_measure_ch(EAST,  cpulse, pulse_len, adc_buffer[EAST], ADC_SAMPLE_LEN, true);
        sig_level[WEST] = ane_measure_ch(WEST, cpulse, pulse_len, adc_buffer[WEST], ADC_SAMPLE_LEN, true);

        // test only
        if(is_ane_proc)
            send_to_processing(1, 0, ADC_SAMPLE_LEN-50, cpulse, pulse_len);

        // to record the runtime
        rt_tick_t tick = rt_tick_get();

        float dt[4] = {0};
        bool is_data_correct = true;
        for(int idx = 0; idx < 4; idx ++)
        {
            // convert to float
            preprocess(adc_buffer[idx], sig2, sig_level[idx], ADC_SAMPLE_LEN);
            // band pass filter.
            filter(sig2, sig, ADC_SAMPLE_LEN, bp_coeff, bp_coeff_order);
            // normalize to -1 to 1
            normalize(&sig[DEADZONE_OFFSET], VALID_LEN);

            // Beside to use the signal peak to calculate the rough propagation time,
            // We use a few more peak and valley around the main peaks.
            // And use MSE to match the signals.  This is a shape detector.
            // detect peaks as shape.
            float shape[PEAK_LEN][2];
            memset(shape, 0, sizeof(shape));
            capture_peaks(&sig[DEADZONE_OFFSET], VALID_LEN, shape, PEAK_LEFT, PEAK_RIGHT, 0.2);

            // use peak to find the offset if there is any on the main peak
            #define MSE_RANGE 9
            float mse[MSE_RANGE];
            int mini_mse;
            int peak_off;
            mini_mse = match_shape(ref_shape[idx], shape, PEAK_LEN, mse, MSE_RANGE);
            // use linear functions to locate the main peak, this is different from the mse method in realtime measurement.
            //peak_off = locate_main_peak(shape, PEAK_LEN);
            peak_off =  mini_mse - MSE_RANGE/2;
            mse_history[idx] = 0.9*mse_history[idx] + 0.1*mse[mini_mse];
            if(isnanf(mse[0])){
                err = ERR_MSE_NAN;
                is_data_correct = false;
            }
            if(mse[mini_mse] > mse_history[idx]*10)
            {
                if(is_ane_log)
                    LOG_W("cannot match signal, mse history:%f, mini mse: %f", mse_history[idx], mse[mini_mse]);
                err = ERR_SHAPE_MISMATCH;
                is_data_correct = false;
            }
            // dynamically update the ref_shapes
//            else if(!peak_off){
//                update_shape(ref_shape[idx], shape, 0.02);
//            }

            // finally we can locate the main peak, despite the peak is distorted
            if(is_ane_log && abs(peak_off) > 2){ // small offset dose not considered as error
                int buf_idx = 0;
                for(int i=0; i<MSE_RANGE; i++)
                    buf_idx += sprintf(&str_buf[buf_idx],"%f ",mse[i]);
                LOG_W("peak offset %d, ch: %s, mse: %s",peak_off, ane_ch_names[idx], str_buf);
            }

            // we start the crossing point from the PEAK_ZC + offset detected by shape
            int off = shape[PEAK_ZC + peak_off][0];
            float zero_cross[ZEROCROSS_LEN] = {0};
            linear_interpolation_zerocrossing(&sig[DEADZONE_OFFSET + off], VALID_LEN-off, zero_cross, ZEROCROSS_LEN);
            // recover the offsets for these zero cross
            for(int j=0; j<ZEROCROSS_LEN; j++)
                zero_cross[j] += off + DEADZONE_OFFSET;

            // finally, uses a few zero crossing points to calculate the propagation time.
            dt[idx] = average(zero_cross, NUM_ZC_AVG);
            dt[idx] += pulse_offset[idx];
        }

        //printf("Sig level: N:%.2f, S:%.2f, E:%.2f, W:%.2f\n", sig_level[NORTH], sig_level[SOUTH], sig_level[EAST], sig_level[WEST]);

        // any channel cannot match the beam shape, then redo the sampling immediately.
        if(!is_data_correct)
        {
            err_count++;
            if(is_ane_log)
                LOG_W("Error count updated: %d, err_code:%d", err_count, err);
            goto cycle_end;
        }

        // convert us to s
        dt[NORTH] /= 1000000.f;
        dt[SOUTH] /= 1000000.f;
        dt[EAST] /= 1000000.f;
        dt[WEST] /= 1000000.f;

        // wind speed.
        ns_v = height / (sin_a * cos_a) * (1.0f/dt[NORTH] - 1.0f/dt[SOUTH]);
        ew_v = height / (sin_a * cos_a) * (1.0f/dt[EAST] - 1.0f/dt[WEST]);
        v = sqrtf(ns_v*ns_v + ew_v*ew_v);

        // sound speed
        ns_c = height / sin_a * (1.0f/dt[NORTH] + 1.0f/dt[SOUTH]);
        ew_c = height / sin_a * (1.0f/dt[EAST] + 1.0f/dt[WEST]);
        c = (ns_c + ew_c)/2;

        // a basic hard check
        if(270 > c || c > 365)
        {
            err = ERR_WINDSPEED;
            if(is_ane_log)
                LOG_W("Wind speed abnormal, ns:%.1f, ew:%.1f, est_c:%.1f, err_count:%d", ns_c, ew_c, est_c, err_count);
            goto cycle_end;
        }

        if(c_history == 0)
            c_history = c;
        c_history = c_history*0.9 + c*0.1;

        // course
        course = atan2f(-ew_v, -ns_v)/3.1415926*180 + 180;

        // sound speed.
        est_c = speed_of_sound_from_T(air_info.temperature);

        // final check, if wind speed abnormal, then resample
        if(fabs(est_c - c) > 10 || fabs(c - c_history) > 5)
        {
            err = ERR_WINDSPEED;
            err_count++;
            goto cycle_end;
        }

        // data output
        c_acc += c;
        ns_v_acc += ns_v;
        ew_v_acc += ew_v;
        oversampling_count ++;

        if(oversampling_count >= cfg->oversampling){
            ns_v_acc /= oversampling_count;
            ew_v_acc /= oversampling_count;
            c_acc /= oversampling_count;

            anemometer.speed = sqrtf(ns_v_acc*ns_v_acc + ew_v_acc*ew_v_acc);;
            anemometer.soundspeed = c_acc;
            if(anemometer.speed >= 0.25)
                anemometer.course = atan2f(-ew_v_acc, -ns_v_acc)/3.1415926*180 + 180;
            else
                anemometer.course = -1;

            ringbuffer_add(&wind_hist, anemometer.speed);
            anemometer.speed30savg = ringbuffer_average(&wind_hist);
            anemometer.speed30smax = ringbuffer_max(&wind_hist);

            data_updated(&anemometer.info);

            // reset
            ns_v_acc = 0;
            ew_v_acc = 0;
            c_acc = 0;
            oversampling_count = 0;
            //printf("%4.1f, %4.1f, %7.3f,\n", anemometer.soundspeed, anemometer.course, anemometer.speed);
        }

        //LOG_I("run time %d ms", rt_tick_get() - tick); //19~30ms
        if(is_ane_log)
        {
            printf("Course=%5.1fdeg, V=%5.2fm/s, C=%5.1fm/s, ns=%5.2fm/s, ew=%5.2fm/s\n", course, v, c, ns_v, ew_v);
            //printf("Course=%3.1fdeg, %3.1f, %2.3f,\n",course, c, v);
            //printf("%3.1f, %2.3f,\n", c, v);
        }

cycle_end:
        // err code
        anemometer.err_code = err;

        // dump last adc masurement if error.
        if(err != NORMAL)
        {
            // allow only one dump per second.
            if(rt_tick_get() - last_dump > RT_TICK_PER_SECOND &&
                    ane_cfg->is_dump_error){
                LOG_W("Dumping error, error code : %d", err);
                last_dump = rt_tick_get();
                dump_error_measurement(err_count);
            }
            // if there is error, redo the measurement.
            continue;
        }

        // frequency control
        rt_thread_mdelay(period - rt_tick_get() % period);
    }
}


int thread_anemometer_init()
{
    rt_thread_t tid;
    tid = rt_thread_create("anemo", thread_anemometer, RT_NULL, 1024*4, 24, 1000);
    if(!tid)
        return RT_ERROR;
    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(thread_anemometer_init);
