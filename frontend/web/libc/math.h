/* math.h — what the library calls. sqrt and floor are instructions in
 * WebAssembly; pow comes from the page. */
#ifndef FR_WEB_MATH_H
#define FR_WEB_MATH_H
double sqrt(double x);
float  sqrtf(float x);
double floor(double x);
double ceil(double x);
double fabs(double x);
float  fabsf(float x);
double round(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
#endif
