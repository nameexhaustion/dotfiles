// burn cpu [threads]  - FMA-heavy NEON loop on every core (or N threads) until killed
// burn gpu            - OpenCL FMA kernel dispatched back to back (~100 ms each) until killed
// Build (aarch64 only, NEON): gcc -O2 -pthread -o burn-aarch64 burn.c -ldl
#include <arm_neon.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile double sink;

static void *cpu_worker(void *arg) {
    (void)arg;
    float64x2_t a[12], m = vdupq_n_f64(1.0000001), c = vdupq_n_f64(1e-9);
    for (int i = 0; i < 12; i++) a[i] = vdupq_n_f64(i + 1.0);
    for (;;) {
        for (int n = 0; n < 1 << 20; n++)
            for (int i = 0; i < 12; i++) a[i] = vfmaq_f64(c, a[i], m);
        for (int i = 0; i < 12; i++) a[i] = vminq_f64(a[i], vdupq_n_f64(1e6));
        sink = vgetq_lane_f64(a[0], 0);
    }
    return NULL;
}

static int burn_cpu(int threads) {
    if (threads <= 0) threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    pthread_t t;
    for (int i = 1; i < threads; i++) pthread_create(&t, NULL, cpu_worker, NULL);
    fprintf(stderr, "burn cpu: %d threads\n", threads);
    cpu_worker(NULL);
    return 0;
}

// Minimal OpenCL 1.2 API, loaded at run time (no headers installed).
typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef void *cl_ptr;
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_DEVICE_NAME 0x102B
#define CL_MEM_WRITE_ONLY (1 << 1)
#define CL_PROGRAM_BUILD_LOG 0x1183

#define CLFN(ret, name, args) static ret(*name) args;
CLFN(cl_int, clGetPlatformIDs, (cl_uint, cl_ptr *, cl_uint *))
CLFN(cl_int, clGetDeviceIDs, (cl_ptr, uint64_t, cl_uint, cl_ptr *, cl_uint *))
CLFN(cl_int, clGetDeviceInfo, (cl_ptr, cl_uint, size_t, void *, size_t *))
CLFN(cl_ptr, clCreateContext, (const intptr_t *, cl_uint, const cl_ptr *, void *, void *, cl_int *))
CLFN(cl_ptr, clCreateCommandQueue, (cl_ptr, cl_ptr, uint64_t, cl_int *))
CLFN(cl_ptr, clCreateProgramWithSource, (cl_ptr, cl_uint, const char **, const size_t *, cl_int *))
CLFN(cl_int, clBuildProgram, (cl_ptr, cl_uint, const cl_ptr *, const char *, void *, void *))
CLFN(cl_int, clGetProgramBuildInfo, (cl_ptr, cl_ptr, cl_uint, size_t, void *, size_t *))
CLFN(cl_ptr, clCreateKernel, (cl_ptr, const char *, cl_int *))
CLFN(cl_ptr, clCreateBuffer, (cl_ptr, uint64_t, size_t, void *, cl_int *))
CLFN(cl_int, clSetKernelArg, (cl_ptr, cl_uint, size_t, const void *))
CLFN(cl_int, clEnqueueNDRangeKernel, (cl_ptr, cl_ptr, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_ptr *, cl_ptr *))
CLFN(cl_int, clFinish, (cl_ptr))

static const char *kernel_src =
    "__kernel void burn(__global float *out, int iters) {\n"
    "  float4 a = (float4)(get_global_id(0) * 1e-6f, 1.0f, 2.0f, 3.0f);\n"
    "  float4 b = a + 0.5f, c = a + 1.5f, d = a + 2.5f;\n"
    "  const float4 m = (float4)(0.9999f), k = (float4)(1e-4f);\n"
    "  for (int i = 0; i < iters; i++) {\n"
    "    a = mad(a, m, k); b = mad(b, m, k); c = mad(c, m, k); d = mad(d, m, k);\n"
    "    a = mad(a, m, k); b = mad(b, m, k); c = mad(c, m, k); d = mad(d, m, k);\n"
    "  }\n"
    "  float4 s = a + b + c + d;\n"
    "  out[get_global_id(0)] = s.x + s.y + s.z + s.w;\n"
    "}\n";

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define CHECK(x) do { cl_int e_ = (x); if (e_) { fprintf(stderr, "%s failed: %d\n", #x, e_); return 1; } } while (0)

static int burn_gpu(void) {
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen libOpenCL.so.1: %s\n", dlerror()); return 1; }
#define LOAD(name) if (!(*(void **)&name = dlsym(h, #name))) { fprintf(stderr, "missing %s\n", #name); return 1; }
    LOAD(clGetPlatformIDs) LOAD(clGetDeviceIDs) LOAD(clGetDeviceInfo) LOAD(clCreateContext)
    LOAD(clCreateCommandQueue) LOAD(clCreateProgramWithSource) LOAD(clBuildProgram)
    LOAD(clGetProgramBuildInfo) LOAD(clCreateKernel) LOAD(clCreateBuffer) LOAD(clSetKernelArg)
    LOAD(clEnqueueNDRangeKernel) LOAD(clFinish)

    cl_ptr plat, dev;
    cl_int err;
    CHECK(clGetPlatformIDs(1, &plat, NULL));
    CHECK(clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &dev, NULL));
    char name[256] = "";
    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof name, name, NULL);
    cl_ptr ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err); CHECK(err);
    cl_ptr q = clCreateCommandQueue(ctx, dev, 0, &err); CHECK(err);
    cl_ptr prog = clCreateProgramWithSource(ctx, 1, &kernel_src, NULL, &err); CHECK(err);
    if (clBuildProgram(prog, 1, &dev, "", NULL, NULL)) {
        char log[4096] = "";
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL);
        fprintf(stderr, "build failed:\n%s\n", log);
        return 1;
    }
    cl_ptr k = clCreateKernel(prog, "burn", &err); CHECK(err);
    size_t global = 1 << 20;
    cl_ptr buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, global * sizeof(float), NULL, &err); CHECK(err);
    CHECK(clSetKernelArg(k, 0, sizeof buf, &buf));

    // Grow iterations until one dispatch takes ~100 ms, so the GPU stays saturated while a
    // kill still lands quickly and the display compositor gets slices in between.
    int iters = 64;
    fprintf(stderr, "burn gpu: %s\n", name);
    for (;;) {
        CHECK(clSetKernelArg(k, 1, sizeof iters, &iters));
        double t0 = now();
        CHECK(clEnqueueNDRangeKernel(q, k, 1, NULL, &global, NULL, 0, NULL, NULL));
        CHECK(clFinish(q));
        double dt = now() - t0;
        if (dt < 0.05 && iters < 1 << 20) iters *= 2;
        else if (dt > 0.2 && iters > 16) iters /= 2;
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "cpu")) return burn_cpu(argc >= 3 ? atoi(argv[2]) : 0);
    if (argc >= 2 && !strcmp(argv[1], "gpu")) return burn_gpu();
    fprintf(stderr, "usage: burn cpu [threads] | burn gpu\n");
    return 2;
}
