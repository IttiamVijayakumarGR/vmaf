/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "common/macros.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "mem.h"

#include "picture.h"

typedef struct VifState {
    size_t buf_stride;          //stride size for intermidate buffers
    size_t buf_frame_size;      //size of frame buffer
    pixel *buf;                 //buffer for vif intermidiate data calculations
    uint16_t log_values[65537]; //log value table for integer implementation

    uint16_t *integer_ref_scale;
    uint16_t *integer_dis_scale;
    uint16_t *integer_mu1;
    uint16_t *integer_mu2;
    uint16_t *integer_mu1_32;
    uint16_t *integer_mu2_32;
    uint32_t *integer_ref_sq_filt;
    uint32_t *integer_dis_sq_filt;
    uint32_t *integer_ref_dis_filt;
    uint32_t *tmp_ref_convol;
    uint32_t *tmp_dis_convol;
    uint32_t *tmp_mu1;
    uint32_t *tmp_mu2;
    uint32_t *tmp_ref;
    uint32_t *tmp_dis;
    uint32_t *tmp_ref_dis;

} VifState;

static const uint16_t vif_filter1d_table[4][17] = {
    { 489, 935, 1640, 2640, 3896, 5274, 6547, 7455, 7784, 7455, 6547, 5274, 3896, 2640, 1640, 935, 489 },
    { 1244, 3663, 7925, 12590, 14692, 12590, 7925, 3663, 1244 },
    { 3571, 16004, 26386, 16004, 3571 },
    { 10904, 43728, 10904 }
};

static const int vif_filter1d_width[4] = { 17, 9, 5, 3 };

 /*
  * Works similar to floating-point function vif_dec2_s
  */
static void vif_dec2(const uint16_t *src, uint16_t *dst, int src_w, int src_h,
                     int src_stride, int dst_stride)
{
    int src_px_stride = src_stride >> 2;  //divide by sizeof(uint32_t)
    int dst_px_stride = dst_stride >> 2;  //divide by sizeof(uint32_t)

    // decimation by 2 in each direction (after gaussian blur? check)
    uint16_t *a = src;
    uint16_t *b = dst;
    for (unsigned i = 0; i < src_h / 2; ++i) {
        for (unsigned j = 0; j < src_w / 2; ++j) {
            b[j] =  a[j * 2];
        }
        b += src_px_stride;
        a += src_px_stride * 2;
    }
}

/*
 * calculate best 16 bit to represent the input and no of shifts required to get best 16 bit
 */
FORCE_INLINE static inline uint16_t
integer_get_best_16bits_opt(uint32_t temp, int *x)
{
    int k = __builtin_clz(temp); // for int

    if (k > 16)  // temp < 2^15
    {
        k -= 16;
        temp = temp << k;
        *x = k;

    }
    else if (k < 15)  // temp > 2^16
    {
        k = 16 - k;
        temp = temp >> k;
        *x = -k;
    }
    else
    {
        *x = 0;
        if (temp >> 16)
        {
            temp = temp >> 1;
            *x = -1;
        }
    }

    return temp;
}

/*
 * divide integer_get_best_16bits_opt for more improved performance as for input greater than 16 bit
 */
FORCE_INLINE static inline uint16_t
integer_get_best_16bits_opt_greater(uint32_t temp, int *x)
{
    int k = __builtin_clz(temp); // for int
    k = 16 - k;
    temp = temp >> k;
    *x = -k;
    return temp;
}

/*
 * Works similar to integer_get_best_16bits_opt function but for 64 bit input
 */
FORCE_INLINE static inline uint16_t
integer_get_best_16bits_opt_64(uint64_t temp, int *x)
{
    int k = __builtin_clzll(temp); // for long

    if (k > 48)  // temp < 2^47
    {
        k -= 48;
        temp = temp << k;
        *x = k;

    }
    else if (k < 47)  // temp > 2^48
    {
        k = 48 - k;
        temp = temp >> k;
        *x = -k;
    }
    else
    {
        *x = 0;
        if (temp >> 16)
        {
            temp = temp >> 1;
            *x = -1;
        }
    }

    return (uint16_t)temp;
}

/*
 * divide integer_get_best_16bits_opt_64 for more improved performance as for input greater than 16 bit
 */
FORCE_INLINE static inline uint16_t
integer_get_best_16bits_opt_greater_64(uint64_t temp, int *x)
{
    int k = __builtin_clzll(temp); // for long
    k = 16 - k;
    temp = temp >> k;
    *x = -k;
    return (uint16_t)temp;
}

static void vif_filter1d_combined_8(const uint16_t *integer_filter, const uint8_t *integer_curr_ref_scale,
                                    const uint8_t *integer_curr_dis_scale, uint32_t *integer_ref_sq_filt,
                                    uint32_t *integer_dis_sq_filt, uint32_t *integer_ref_dis_filt, uint32_t *integer_mu1,
                                    uint32_t *integer_mu2, int w, int h, int curr_ref_stride, int curr_dis_stride,
                                    int dst_stride, int fwidth, int scale, uint32_t *tmp_mu1, uint32_t *tmp_mu2,
                                    uint32_t *tmp_ref, uint32_t *tmp_dis, uint32_t *tmp_ref_dis, int inp_size_bits)
{
    int src_px_stride1 = curr_ref_stride >> 2;  //divide by sizeof(uint32_t)
    int src_px_stride2 = curr_dis_stride >> 2;  //divide by sizeof(uint32_t)
    int dst_px_stride = dst_stride >> 2;        //divide by sizeof(uint32_t)

    const int32_t shift_HorizontalPass = 16;
    const int32_t add_bef_shift_round_HP = 32768;
    const int32_t add_bef_shift_round_VP = (int32_t)pow(2, (inp_size_bits - 1));
    const int32_t shift_VerticalPass = inp_size_bits;
    const int32_t shift_VerticalPass_square = (inp_size_bits - 8) * 2;
    const int32_t add_bef_shift_round_VP_square =
        (inp_size_bits == 8) ? 0 : (int32_t)pow(2, shift_VerticalPass_square - 1);

    for (unsigned i = 0; i < h; ++i) {
        // vertical
        for (unsigned j = 0; j < w; ++j) {
            uint64_t accum_ref = 0;
            uint64_t accum_dis = 0;
            uint64_t accum_ref_dis = 0;
            uint32_t accum_mu1 = 0;
            uint32_t accum_mu2 = 0;

            for (unsigned fi = 0; fi < fwidth; ++fi) {
                int ii = i - fwidth / 2 + fi;
                ii = ii < 0 ? -ii : (ii >= h ? 2 * h - ii - 1 : ii);

                uint16_t imgcoeff_ref = integer_curr_ref_scale[ii * src_px_stride1 + j];
                uint16_t imgcoeff_dis = integer_curr_dis_scale[ii * src_px_stride2 + j];

                uint32_t img_coeff_ref_sq = (uint32_t)imgcoeff_ref * imgcoeff_ref;
                uint32_t img_coeff_dis_sq = (uint32_t)imgcoeff_dis * imgcoeff_dis;
                uint32_t img_coeff_ref_dis_sq = (uint32_t)imgcoeff_ref * imgcoeff_dis;

                uint32_t fcoeff = integer_filter[fi];
                accum_mu1 += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_mu2 += fcoeff * ((uint32_t)imgcoeff_dis);
                accum_ref += (uint64_t)fcoeff * img_coeff_ref_sq;
                accum_dis += (uint64_t)fcoeff * img_coeff_dis_sq;
                accum_ref_dis += (uint64_t)fcoeff * img_coeff_ref_dis_sq;
            }

            // For scale 0 accum is Q32 as imgcoeff is Q8 and fcoeff is Q16
            // For scale 1,2,3 accum is Q48 as both imgcoeff and fcoeff are Q16
            tmp_mu1[j] = (uint16_t)((accum_mu1 + add_bef_shift_round_VP) >> shift_VerticalPass);
            tmp_mu2[j] = (uint16_t)((accum_mu2 + add_bef_shift_round_VP) >> shift_VerticalPass);
            tmp_ref[j] = (uint32_t)((accum_ref + add_bef_shift_round_VP_square) >> shift_VerticalPass_square);
            tmp_dis[j] = (uint32_t)((accum_dis + add_bef_shift_round_VP_square) >> shift_VerticalPass_square);
            tmp_ref_dis[j] = (uint32_t)((accum_ref_dis + add_bef_shift_round_VP_square) >> shift_VerticalPass_square);
        }

        // horizontal
        for (unsigned j = 0; j < w; ++j) {
            uint64_t accum_ref = 0;
            uint64_t accum_dis = 0;
            uint64_t accum_ref_dis = 0;
            uint32_t accum_mu1 = 0;
            uint32_t accum_mu2 = 0;

            for (unsigned fj = 0; fj < fwidth; ++fj) {
                int jj = j - fwidth / 2 + fj;
                jj = jj < 0 ? -jj : (jj >= w ? 2 * w - jj - 1 : jj);

                uint16_t imgcoeff_ref = (uint16_t)tmp_mu1[jj];
                uint16_t imgcoeff_dis = (uint16_t)tmp_mu2[jj];

                uint32_t fcoeff = integer_filter[fj];
                accum_mu1 += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_mu2 += fcoeff * ((uint32_t)imgcoeff_dis);
                accum_ref += fcoeff * ((uint64_t)tmp_ref[jj]);
                accum_dis += fcoeff * ((uint64_t)tmp_dis[jj]);
                accum_ref_dis += fcoeff * ((uint64_t)tmp_ref_dis[jj]);
            }

            // For scale 0 accum is Q48 as tmp is Q32 and fcoeff is Q16. Hence accum is right shifted by 16 to make dst Q32
            // For scale 1,2,3 accum is Q64 as tmp is Q48 and fcoeff is Q16. Hence accum is right shifted by 32 to make dst Q32
            integer_mu1[i * dst_px_stride + j] = accum_mu1;
            integer_mu2[i * dst_px_stride + j] = accum_mu2;
            integer_ref_sq_filt[i * dst_px_stride + j] = (uint32_t)((accum_ref + add_bef_shift_round_HP) >> shift_HorizontalPass);
            integer_dis_sq_filt[i * dst_px_stride + j] = (uint32_t)((accum_dis + add_bef_shift_round_HP) >> shift_HorizontalPass);
            integer_ref_dis_filt[i * dst_px_stride + j] = (uint32_t)((accum_ref_dis + add_bef_shift_round_HP) >> shift_HorizontalPass);
        }
    }
}

static void vif_filter1d_combined_16(const uint16_t *integer_filter, const uint16_t *integer_curr_ref_scale,
                                     const uint16_t *integer_curr_dis_scale, uint32_t *integer_ref_sq_filt,
                                     uint32_t *integer_dis_sq_filt, uint32_t *integer_ref_dis_filt, uint32_t *integer_mu1,
                                     uint32_t *integer_mu2, int w, int h, int curr_ref_stride, int curr_dis_stride,
                                     int dst_stride, int fwidth, int scale, uint32_t *tmp_mu1, uint32_t *tmp_mu2,
                                     uint32_t *tmp_ref, uint32_t *tmp_dis, uint32_t *tmp_ref_dis, int inp_size_bits)
{
    int src_px_stride1 = curr_ref_stride >> 2;  //divide by sizeof(uint32_t)
    int src_px_stride2 = curr_dis_stride >> 2;  //divide by sizeof(uint32_t)
    int dst_px_stride = dst_stride >> 2;        //divide by sizeof(uint32_t)

    int32_t add_bef_shift_round_HP, shift_HorizontalPass;
    int32_t add_bef_shift_round_VP, shift_VerticalPass;
    int32_t add_bef_shift_round_VP_square, shift_VerticalPass_square;
    if (scale == 0)
    {
        shift_HorizontalPass = 16;
        add_bef_shift_round_HP = 32768;
        add_bef_shift_round_VP = (int32_t)pow(2, (inp_size_bits - 1));
        shift_VerticalPass = inp_size_bits;
        shift_VerticalPass_square = (inp_size_bits - 8) * 2;
        add_bef_shift_round_VP_square = (inp_size_bits == 8) ? 0 : (int32_t)pow(2, shift_VerticalPass_square - 1);
    }
    else
    {
        add_bef_shift_round_HP = 32768;
        shift_HorizontalPass = 16;
        add_bef_shift_round_VP = 32768;
        shift_VerticalPass = 16;
        add_bef_shift_round_VP_square = 32768;
        shift_VerticalPass_square = 16;
    }

    uint32_t fcoeff;
    uint16_t imgcoeff_ref, imgcoeff_dis;

    int i, j, fi, fj, ii, jj;
    for (i = 0; i < h; ++i) {
        /* Vertical pass. */
        for (j = 0; j < w; ++j) {
            uint64_t accum_ref = 0;
            uint64_t accum_dis = 0;
            uint64_t accum_ref_dis = 0;
            uint32_t accum_mu1 = 0;
            uint32_t accum_mu2 = 0;

            for (fi = 0; fi < fwidth; ++fi) {
                fcoeff = integer_filter[fi];
                ii = i - fwidth / 2 + fi;
                ii = ii < 0 ? -ii : (ii >= h ? 2 * h - ii - 1 : ii);

                imgcoeff_ref = integer_curr_ref_scale[ii * src_px_stride1 + j];
                imgcoeff_dis = integer_curr_dis_scale[ii * src_px_stride2 + j];

                uint32_t img_coeff_ref_sq = (uint32_t)imgcoeff_ref * imgcoeff_ref;
                uint32_t img_coeff_dis_sq = (uint32_t)imgcoeff_dis * imgcoeff_dis;
                uint32_t img_coeff_ref_dis_sq = (uint32_t)imgcoeff_ref * imgcoeff_dis;

                accum_mu1 += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_mu2 += fcoeff * ((uint32_t)imgcoeff_dis);
                accum_ref += (uint64_t)fcoeff * img_coeff_ref_sq;
                accum_dis += (uint64_t)fcoeff * img_coeff_dis_sq;
                accum_ref_dis += (uint64_t)fcoeff * img_coeff_ref_dis_sq;
            }
            /**
                * For scale 0 accum is Q32 as imgcoeff is Q8 and fcoeff is Q16
                * For scale 1,2,3 accum is Q48 as both imgcoeff and fcoeff are Q16
                */
            tmp_mu1[j] = (uint16_t)((accum_mu1 + add_bef_shift_round_VP) >> shift_VerticalPass);
            tmp_mu2[j] = (uint16_t)((accum_mu2 + add_bef_shift_round_VP) >> shift_VerticalPass);
            tmp_ref[j] = (uint32_t)((accum_ref + add_bef_shift_round_VP_square) >> shift_VerticalPass_square);
            tmp_dis[j] = (uint32_t)((accum_dis + add_bef_shift_round_VP_square) >> shift_VerticalPass_square);
            tmp_ref_dis[j] = (uint32_t)((accum_ref_dis + add_bef_shift_round_VP_square) >> shift_VerticalPass_square);
        }
        /* Horizontal pass. */
        for (j = 0; j < w; ++j) {
            uint64_t accum_ref = 0;
            uint64_t accum_dis = 0;
            uint64_t accum_ref_dis = 0;
            uint32_t accum_mu1 = 0;
            uint32_t accum_mu2 = 0;

            for (fj = 0; fj < fwidth; ++fj) {
                fcoeff = integer_filter[fj];

                jj = j - fwidth / 2 + fj;
                jj = jj < 0 ? -jj : (jj >= w ? 2 * w - jj - 1 : jj);

                imgcoeff_ref = (uint16_t)tmp_mu1[jj];
                imgcoeff_dis = (uint16_t)tmp_mu2[jj];

                accum_mu1 += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_mu2 += fcoeff * ((uint32_t)imgcoeff_dis);
                accum_ref += fcoeff * ((uint64_t)tmp_ref[jj]);
                accum_dis += fcoeff * ((uint64_t)tmp_dis[jj]);
                accum_ref_dis += fcoeff * ((uint64_t)tmp_ref_dis[jj]);
            }
            /**
                * For scale 0 accum is Q48 as tmp is Q32 and fcoeff is Q16. Hence accum is right shifted by 16 to make dst Q32
                * For scale 1,2,3 accum is Q64 as tmp is Q48 and fcoeff is Q16. Hence accum is right shifted by 32 to make dst Q32
                */

            integer_mu1[i * dst_px_stride + j] = accum_mu1;
            integer_mu2[i * dst_px_stride + j] = accum_mu2;
            integer_ref_sq_filt[i * dst_px_stride + j] = (uint32_t)((accum_ref + add_bef_shift_round_HP) >> shift_HorizontalPass);
            integer_dis_sq_filt[i * dst_px_stride + j] = (uint32_t)((accum_dis + add_bef_shift_round_HP) >> shift_HorizontalPass);
            integer_ref_dis_filt[i * dst_px_stride + j] = (uint32_t)((accum_ref_dis + add_bef_shift_round_HP) >> shift_HorizontalPass);
        }
    }
}

static void vif_statistic(const uint32_t *mu1, const uint32_t *mu2, const uint32_t *xx_filt,
                          const uint32_t *yy_filt, const uint32_t *xy_filt, float *num, float *den,
                          int w, int h, int mu1_stride, int scale, uint16_t *log_values)
{
    static const int32_t sigma_nsq = 65536 << 1;  //Float equivalent of same is 2. (2 * 65536)

    static const int16_t sigma_max_inv = 2; //This is used as shift variable, hence it will be actually multiplied by 4. In float it is equivalent to 4/(256*256) but actually in float 4/(255*255) is used
    int mu1_sq_px_stride = mu1_stride >> 2; //divide by sizeof(uint32_t)
    //as stride are same for all of these no need for multiple calculation
    int mu2_sq_px_stride = mu1_sq_px_stride;
    int mu1_mu2_px_stride = mu1_sq_px_stride;
    int xx_filt_px_stride = mu1_sq_px_stride;
    int yy_filt_px_stride = mu1_sq_px_stride;
    int xy_filt_px_stride = mu1_sq_px_stride;
    int num_px_stride = mu1_sq_px_stride;
    int den_px_stride = mu1_sq_px_stride;

    uint32_t mu1_val, mu2_val;
    uint32_t mu1_sq_val, mu2_sq_val, mu1_mu2_val, xx_filt_val, yy_filt_val, xy_filt_val;
    int32_t sigma1_sq, sigma2_sq, sigma12;
    int64_t num_val, den_val;
    int i, j;
    int64_t accum_x = 0, accum_x2 = 0;

    int64_t num_accum_x = 0;
    int64_t accum_num_log = 0.0;
    int64_t accum_den_log = 0.0;

    int64_t accum_num_non_log = 0;
    int64_t accum_den_non_log = 0;
    /**
     * In floating-point there are two types of numerator scores and denominator scores
     * 1. num = 1 - sigma1_sq * constant den =1  when sigma1_sq<2  here constant=4/(255*255)
     * 2. num = log2(((sigma2_sq+2)*sigma1_sq)/((sigma2_sq+2)*sigma1_sq-sigma12*sigma12) den=log2(1+(sigma1_sq/2)) else
     *
     * In fixed-point separate accumulator is used for non-log score accumulations and log-based score accumulation
     * For non-log accumulator of numerator, only sigma1_sq * constant in fixed-point is accumulated
     * log based values are separately accumulated.
     * While adding both accumulator values the non-log accumulator is converted such that it is equivalent to 1 - sigma1_sq * constant(1's are accumulated with non-log denominator accumulator)
    */
    for (i = 0; i < h; ++i) {
        for (j = 0; j < w; ++j) {
            mu1_val = mu1[i * mu1_sq_px_stride + j];
            mu2_val = mu2[i * mu1_sq_px_stride + j];
            mu1_sq_val = (uint32_t)((((uint64_t)mu1_val * mu1_val) + 2147483648) >> 32); // same name as the Matlab code vifp_mscale.m
            mu2_sq_val = (uint32_t)((((uint64_t)mu2_val * mu2_val) + 2147483648) >> 32); // shift to convert form Q64 to Q32
            mu1_mu2_val = (uint32_t)((((uint64_t)mu1_val * mu2_val) + 2147483648) >> 32);

            xx_filt_val = xx_filt[i * xx_filt_px_stride + j];
            yy_filt_val = yy_filt[i * yy_filt_px_stride + j];

            sigma1_sq = (int32_t)(xx_filt_val - mu1_sq_val);
            sigma2_sq = (int32_t)(yy_filt_val - mu2_sq_val);

            if (sigma1_sq >= sigma_nsq) {
                xy_filt_val = xy_filt[i * xy_filt_px_stride + j];
                sigma12 = (int32_t)(xy_filt_val - mu1_mu2_val);

                uint32_t log_den_stage1 = (uint32_t)(sigma_nsq + sigma1_sq);
                int x;

                //Best 16 bits are taken to calculate the log functions return value is between 16384 to 65535
                uint16_t log_den1 = integer_get_best_16bits_opt_greater(log_den_stage1, &x);
                /**
                 * log values are taken from the look-up table generated by
                 * log_generate() function which is called in integer_combo_threadfunc
                 * den_val in float is log2(1 + sigma1_sq/2)
                 * here it is converted to equivalent of log2(2+sigma1_sq) - log2(2) i.e log2(2*65536+sigma1_sq) - 17
                 * multiplied by 2048 as log_value = log2(i)*2048 i=16384 to 65535 generated using log_value
                 * x because best 16 bits are taken
                 */
                 //den_val = log_values[log_den1 - 16384] - (int64_t)(x + 17) * 2048; //2048 is because log is calculated as log=log2(i)*2048
                 //den_val = log_values[log_den1 ] - (int64_t)(x + 17) * 2048; //2048 is because log is calculated as log=log2(i)*2048

                //den_val = log_values[log_den1] - (int64_t)((x + 17) << 11 ); //2048 is because log is calculated as log=log2(i)*2048

                //accumulate no of pixel to reduce shifting and addition operation to be done at once

                num_accum_x++;
                accum_x += x;   //accumulate value
                den_val = log_values[log_den1];

                if (sigma12 >= 0) {
                    /**
                     * In floating-point numerator = log2(sv_sq/g)
                     * and sv_sq = (sigma2_sq+sigma_nsq)*sigma1_sq
                     * g = sv_sq - sigma12*sigma12
                     *
                     * In Fixed-point the above is converted to
                     * numerator = log2((sigma2_sq+sigma_nsq)/((sigma2_sq+sigma_nsq)-(sigma12*sigma12/sigma1_sq)))
                     * i.e numerator = log2(sigma2_sq + sigma_nsq)*sigma1_sq - log2((sigma2_sq+sigma_nsq)sigma1_sq-(sigma12*sigma12))
                     */
                    int x1, x2;
                    int32_t numer1 = (sigma2_sq + sigma_nsq);
                    int64_t sigma12_sq = (int64_t)sigma12*sigma12;
                    int64_t numer1_tmp = (int64_t)numer1*sigma1_sq; //numerator
                    uint16_t numlog = integer_get_best_16bits_opt_64((uint64_t)numer1_tmp, &x1);
                    int64_t denom = numer1_tmp - sigma12_sq;
                    if (denom > 0) {
                        //denom>0 is to prevent NaN
                        uint16_t denlog = integer_get_best_16bits_opt_64((uint64_t)denom, &x2);
                        // num_val = log_values[numlog - 16384] - log_values[denlog - 16384] + (int64_t)(x2 - x1) * 2048;

                        //num_val = log_values[numlog ] - log_values[denlog ] + (int64_t)((x2 - x1) << 11);
                        accum_x2 += (x2 - x1);
                        num_val = log_values[numlog] - log_values[denlog];
                        accum_num_log += num_val;
                        accum_den_log += den_val;

                    } else {
                        //This is temporary patch to prevent NaN
                        //num_val = (int64_t)(sigma2_sq << sigma_max_inv);
                        den_val = 1;
                        accum_num_non_log += sigma2_sq;
                        accum_den_non_log += den_val;
                    }
                } else {
                    num_val = 0;
                    accum_num_log += num_val;
                    accum_den_log += den_val;
                }
            } else {
                //num_val = (int64_t)(sigma2_sq << sigma_max_inv);
                den_val = 1;
                accum_num_non_log += sigma2_sq;
                accum_den_non_log += den_val;
            }
        }
    }
    //log has to be divided by 2048 as log_value = log2(i*2048)  i=16384 to 65535
    //num[0] = accum_num_log / 2048.0 + (accum_den_non_log - (accum_num_non_log / 65536.0) / (255.0*255.0));
    //den[0] = accum_den_log / 2048.0 + accum_den_non_log;

    //changed calculation to increase performance
    num[0] = accum_num_log / 2048.0 + accum_x2 + (accum_den_non_log - ((accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = accum_den_log / 2048.0 - (accum_x + (num_accum_x * 17)) + accum_den_non_log;
}

static void
vif_filter1d_rdCombine_8(const uint16_t *integer_filter, const uint8_t *integer_curr_ref_scale,
                         const uint8_t *integer_curr_dis_scale, uint16_t *integer_mu1,
                         uint16_t *integer_mu2, int w, int h, int curr_ref_stride,
                         int curr_dis_stride, int dst_stride, int fwidth, int scale,
                         uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol,
                         int inp_size_bits)
{
    int src_px_stride1 = curr_ref_stride >> 2;  //divide by sizeof(uint32_t)
    int src_px_stride2 = curr_dis_stride >> 2;  //divide by sizeof(uint32_t)
    int dst_px_stride = dst_stride >> 2;        //divide by sizeof(uint32_t)

    int32_t add_bef_shift_round_VP, shift_VerticalPass;
    add_bef_shift_round_VP = (int32_t)pow(2, (inp_size_bits - 1));
    shift_VerticalPass = inp_size_bits;

    uint32_t *tmp_ref = tmp_ref_convol;
    uint32_t *tmp_dis = tmp_dis_convol;
    uint16_t imgcoeff_ref, imgcoeff_dis;
    uint16_t fcoeff;
    int i, j, fi, fj, ii, jj;

    for (i = 0; i < h; ++i) {
        /* Vertical pass. */
        for (j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (fi = 0; fi < fwidth; ++fi) {
                fcoeff = integer_filter[fi];

                ii = i - fwidth / 2 + fi;
                ii = ii < 0 ? -ii : (ii >= h ? 2 * h - ii - 1 : ii);

                imgcoeff_ref = integer_curr_ref_scale[ii * src_px_stride1 + j];
                imgcoeff_dis = integer_curr_dis_scale[ii * src_px_stride2 + j];

                accum_ref += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_dis += fcoeff * ((uint32_t)imgcoeff_dis);
            }

            tmp_ref[j] = (uint16_t)((accum_ref + add_bef_shift_round_VP) >> shift_VerticalPass);
            tmp_dis[j] = (uint16_t)((accum_dis + add_bef_shift_round_VP) >> shift_VerticalPass);
        }

        /* Horizontal pass. */
        for (j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (fj = 0; fj < fwidth; ++fj) {
                fcoeff = integer_filter[fj];

                jj = j - fwidth / 2 + fj;
                jj = jj < 0 ? -jj : (jj >= w ? 2 * w - jj - 1 : jj);

                imgcoeff_ref = (uint16_t)tmp_ref[jj];
                imgcoeff_dis = (uint16_t)tmp_dis[jj];

                accum_ref += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_dis += fcoeff * ((uint32_t)imgcoeff_dis);
            }

            integer_mu1[i * dst_px_stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
            integer_mu2[i * dst_px_stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
}

static void
vif_filter1d_rdCombine_16(const uint16_t *integer_filter, const uint16_t *integer_curr_ref_scale,
                          const uint16_t *integer_curr_dis_scale, uint16_t *integer_mu1,
                          uint16_t *integer_mu2, int w, int h, int curr_ref_stride,
                          int curr_dis_stride, int dst_stride, int fwidth,
                          int scale, uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol,
                          int inp_size_bits)
{
    int src_px_stride1 = curr_ref_stride >> 2;  //divide by sizeof(uint32_t)
    int src_px_stride2 = curr_dis_stride >> 2;  //divide by sizeof(uint32_t)
    int dst_px_stride = dst_stride >> 2;        //divide by sizeof(uint32_t)

    int32_t add_bef_shift_round_VP, shift_VerticalPass;
    if (scale == 0) {
        add_bef_shift_round_VP = (int32_t)pow(2, (inp_size_bits - 1));
        shift_VerticalPass = inp_size_bits;
    } else {
        add_bef_shift_round_VP = 32768;
        shift_VerticalPass = 16;
    }

    uint32_t *tmp_ref = tmp_ref_convol;
    uint32_t *tmp_dis = tmp_dis_convol;
    uint16_t imgcoeff_ref, imgcoeff_dis;
    uint16_t fcoeff;
    int i, j, fi, fj, ii, jj;

    for (i = 0; i < h; ++i) {
        /* Vertical pass. */
        for (j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (fi = 0; fi < fwidth; ++fi) {
                fcoeff = integer_filter[fi];

                ii = i - fwidth / 2 + fi;
                ii = ii < 0 ? -ii : (ii >= h ? 2 * h - ii - 1 : ii);

                imgcoeff_ref = integer_curr_ref_scale[ii * src_px_stride1 + j];
                imgcoeff_dis = integer_curr_dis_scale[ii * src_px_stride2 + j];

                accum_ref += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_dis += fcoeff * ((uint32_t)imgcoeff_dis);
            }

            tmp_ref[j] = (uint16_t)((accum_ref + add_bef_shift_round_VP) >> shift_VerticalPass);
            tmp_dis[j] = (uint16_t)((accum_dis + add_bef_shift_round_VP) >> shift_VerticalPass);
        }

        /* Horizontal pass. */
        for (j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (fj = 0; fj < fwidth; ++fj) {
                fcoeff = integer_filter[fj];

                jj = j - fwidth / 2 + fj;
                jj = jj < 0 ? -jj : (jj >= w ? 2 * w - jj - 1 : jj);

                imgcoeff_ref = (uint16_t)tmp_ref[jj];
                imgcoeff_dis = (uint16_t)tmp_dis[jj];

                accum_ref += fcoeff * ((uint32_t)imgcoeff_ref);
                accum_dis += fcoeff * ((uint32_t)imgcoeff_dis);
            }

            integer_mu1[i * dst_px_stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
            integer_mu2[i * dst_px_stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
}

static const float log2_poly_s[9] = {
    -0.012671635276421, 0.064841182402670, -0.157048836463065,
     0.257167726303123, -0.353800560300520, 0.480131410397451,
    -0.721314327952201, 1.442694803896991, 0
};

static float horner_s(const float *poly, float x, int n)
{
    float var = 0;
    for (unsigned i = 0; i < n; ++i)
        var = var * x + poly[i];
    return var;
}

static float log2f_approx(float x)
{
    const uint32_t exp_zero_const = 0x3F800000UL;
    const uint32_t exp_expo_mask = 0x7F800000UL;
    const uint32_t exp_mant_mask = 0x007FFFFFUL;

    float remain;
    float log_base, log_remain;
    uint32_t u32, u32remain;
    uint32_t exponent, mant;

    if (x == 0)
        return -INFINITY;
    if (x < 0)
        return NAN;

    memcpy(&u32, &x, sizeof(float));

    exponent = (u32 & exp_expo_mask) >> 23;
    mant = (u32 & exp_mant_mask) >> 0;
    u32remain = mant | exp_zero_const;

    memcpy(&remain, &u32remain, sizeof(float));

    log_base = (int32_t)exponent - 127;
    log_remain = horner_s(log2_poly_s, (remain - 1.0f),
                          sizeof(log2_poly_s) / sizeof(float));
    return log_base + log_remain;
}

static void log_generate(uint16_t *log_values)
{
    for (unsigned i = 32767; i < 65536; i++)
        log_values[i] = (uint16_t)round(log2f_approx((float)i) * 2048);
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                unsigned bpc, unsigned w, unsigned h)
{
    VifState *s = fex->priv;

    log_generate(&s->log_values);

    s->buf_stride = ALIGN_CEIL(w * sizeof(uint32_t)); //stride in uint32_t format
    s->buf_frame_size = (size_t)s->buf_stride * h ;

    //7 frame size buffer + 7 stride size buffer for intermidate calculations in integer vif
    s->buf = aligned_malloc((s->buf_frame_size * 7) + (s->buf_stride * 7), MAX_ALIGN);
    if (!s->buf) goto free_buf;

    void *data_top = s->buf;
    s->integer_ref_scale = data_top; data_top += s->buf_frame_size / 2;
    s->integer_dis_scale = data_top; data_top += s->buf_frame_size / 2;
    s->integer_mu1 = data_top; data_top += s->buf_frame_size / 2;
    s->integer_mu2 = data_top; data_top += s->buf_frame_size / 2;
    s->integer_mu1_32 = data_top; data_top += s->buf_frame_size;
    s->integer_mu2_32 = data_top; data_top += s->buf_frame_size;
    s->integer_ref_sq_filt = data_top; data_top += s->buf_frame_size;
    s->integer_dis_sq_filt = data_top; data_top += s->buf_frame_size;
    s->integer_ref_dis_filt = data_top; data_top += s->buf_frame_size;
    s->tmp_ref_convol = data_top; data_top += s->buf_stride;
    s->tmp_dis_convol = data_top; data_top += s->buf_stride;
    s->tmp_mu1 = data_top; data_top += s->buf_stride;
    s->tmp_mu2 = data_top; data_top += s->buf_stride;
    s->tmp_ref = data_top; data_top += s->buf_stride;
    s->tmp_dis = data_top; data_top += s->buf_stride;
    s->tmp_ref_dis = data_top; data_top += s->buf_stride;

    return 0;

free_buf:
    aligned_free(s->buf);
fail:
    return -ENOMEM;
}

static int extract(VmafFeatureExtractor *fex,
                   VmafPicture *ref_pic, VmafPicture *dist_pic,
                   unsigned index, VmafFeatureCollector *feature_collector)
{
    VifState *s = fex->priv;

    void *integer_curr_ref_scale = ref_pic->data[0];
    void *integer_curr_dis_scale = dist_pic->data[0];

    double scores[8];

    ptrdiff_t integer_stride =
        ref_pic->bpc == 8 ? ref_pic->stride[0] << 2 : ref_pic->stride[0] << 1;

    int curr_ref_stride = integer_stride;
    int curr_dis_stride = integer_stride;
    int buf_valid_w = ref_pic->w[0];
    int buf_valid_h = ref_pic->h[0];

    for (unsigned scale = 0; scale < 4; ++scale) {
        const uint16_t *integer_filter = vif_filter1d_table[scale];
        const int filter_width = vif_filter1d_width[scale];

        if (scale > 0) {
            /*
            Combined functionality for both reference and distorted frame in single function
            */
            if (ref_pic->bpc == 8 && scale == 1) {
                vif_filter1d_rdCombine_8(integer_filter, integer_curr_ref_scale,
                                         integer_curr_dis_scale, s->integer_mu1,
                                         s->integer_mu2, buf_valid_w, buf_valid_h, curr_ref_stride,
                                         curr_dis_stride, s->buf_stride, filter_width,
                                         scale - 1, s->tmp_ref_convol, s->tmp_dis_convol,
                                         ref_pic->bpc);
            } else {
                vif_filter1d_rdCombine_16(integer_filter, integer_curr_ref_scale,
                                          integer_curr_dis_scale, s->integer_mu1,
                                          s->integer_mu2, buf_valid_w, buf_valid_h, curr_ref_stride,
                                          curr_dis_stride, s->buf_stride, filter_width,
                                          scale - 1, s->tmp_ref_convol, s->tmp_dis_convol,
                                          ref_pic->bpc);
            }

            vif_dec2(s->integer_mu1, s->integer_ref_scale, buf_valid_w, buf_valid_h,
                     s->buf_stride, s->buf_stride);

            vif_dec2(s->integer_mu2, s->integer_dis_scale, buf_valid_w, buf_valid_h,
                     s->buf_stride, s->buf_stride);

            buf_valid_w /= 2;
            buf_valid_h /= 2;

            integer_curr_ref_scale = s->integer_ref_scale;
            integer_curr_dis_scale = s->integer_dis_scale;

            //after scale 0 reference and distination stride updated
            curr_ref_stride = s->buf_stride;
            curr_dis_stride = s->buf_stride;
        }

        /*
            Combined functionality for both reference and distorted frame in single function
            for vif_filter1d, vif_filter1d_sq and vif_filter1d_ref_dis function
        */
        if (ref_pic->bpc == 8 && scale == 0) {
            vif_filter1d_combined_8(integer_filter, integer_curr_ref_scale,
                                    integer_curr_dis_scale, s->integer_ref_sq_filt,
                                    s->integer_dis_sq_filt, s->integer_ref_dis_filt,
                                    s->integer_mu1_32, s->integer_mu2_32, buf_valid_w, buf_valid_h,
                                    curr_ref_stride, curr_dis_stride, s->buf_stride,
                                    filter_width, scale, s->tmp_mu1, s->tmp_mu2,
                                    s->tmp_ref, s->tmp_dis, s->tmp_ref_dis,
                                    ref_pic->bpc);
        } else {
            vif_filter1d_combined_16(integer_filter, integer_curr_ref_scale,
                                     integer_curr_dis_scale, s->integer_ref_sq_filt,
                                     s->integer_dis_sq_filt, s->integer_ref_dis_filt,
                                     s->integer_mu1_32, s->integer_mu2_32, buf_valid_w, buf_valid_h,
                                     curr_ref_stride, curr_dis_stride, s->buf_stride,
                                     filter_width, scale, s->tmp_mu1, s->tmp_mu2,
                                     s->tmp_ref, s->tmp_dis, s->tmp_ref_dis,
                                     ref_pic->bpc);
        }

        float num_array, den_array;
        vif_statistic(s->integer_mu1_32, s->integer_mu2_32, s->integer_ref_sq_filt,
                      s->integer_dis_sq_filt, s->integer_ref_dis_filt,
                      &num_array, &den_array, buf_valid_w, buf_valid_h,
                      s->buf_stride, scale, s->log_values);
        scores[2 * scale] = num_array;
        scores[2 * scale + 1] = den_array;
    }

    double score_num = 0.0;
    double score_den = 0.0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        score_num += scores[2 * scale];
        score_den += scores[2 * scale + 1];
    }

    double score =
        score_den = 0.0 ? 1.0 : score_num / score_den;

    int err = 0;
    err |= vmaf_feature_collector_append(feature_collector,
                                         "'VMAF_feature_vif_scale0_integer_score'",
                                         scores[0] / scores[1], index);
    err |= vmaf_feature_collector_append(feature_collector,
                                         "'VMAF_feature_vif_scale1_integer_score'",
                                         scores[2] / scores[3], index);
    err |= vmaf_feature_collector_append(feature_collector,
                                         "'VMAF_feature_vif_scale2_integer_score'",
                                         scores[4] / scores[5], index);
    err |= vmaf_feature_collector_append(feature_collector,
                                         "'VMAF_feature_vif_scale3_integer_score'",
                                         scores[6] / scores[7], index);
    return err;
}

static int close(VmafFeatureExtractor *fex)
{
    VifState *s = fex->priv;
    if (s->buf) aligned_free(s->buf);
    return 0;
}

static const char *provided_features[] = {
    "'VMAF_feature_vif_scale0_integer_score'", "'VMAF_feature_vif_scale1_integer_score'",
    "'VMAF_feature_vif_scale2_integer_score'", "'VMAF_feature_vif_scale3_integer_score'",
    NULL
};

VmafFeatureExtractor vmaf_fex_integer_vif = {
    .name = "vif",
    .init = init,
    .extract = extract,
    .close = close,
    .priv_size = sizeof(VifState),
    .provided_features = provided_features,
};
