#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdint.h>
#include "icc_profile.h"
#include "spng.h"

#define INTERMEDIATE_BITS 16  // bit depth of the integer texture given to the encoder
#define TARGET_BITS 10  // quantization bit depth

#define MAXCLL_PERCENTILE 0.9999  // comment out to calculate true MaxCLL instead of top percentile

static float m1 = 1305 / 8192.f;
static float m2 = 2523 / 32.f;
static float c1 = 107 / 128.f;
static float c2 = 2413 / 128.f;
static float c3 = 2392 / 128.f;

float pq_inv_eotf(float y) {
    return powf((c1 + c2 * powf(y, m1)) / (1 + c3 * powf(y, m1)), m2);
}

static const float scrgb_to_bt2100[3][3] = {
        {2939026994.L / 585553224375.L, 9255011753.L / 3513319346250.L, 173911579.L / 501902763750.L},
        {76515593.L / 138420033750.L,   6109575001.L / 830520202500.L,  75493061.L / 830520202500.L},
        {12225392.L / 93230009375.L,    1772384008.L / 2517210253125.L, 18035212433.L / 2517210253125.L},
};

void matrixVectorMult(const float in[3], float out[3], const float matrix[3][3]) {
    for (int i = 0; i < 3; i++) {
        float res = 0;
        for (int j = 0; j < 3; j++) {
            res += matrix[i][j] * in[j];
        }
        out[i] = res;
    }
}

float saturate(float x) {
    return min(1, max(x, 0));
}

typedef struct ThreadData {
    uint8_t *pixels;
    uint16_t *converted;
    uint32_t width;
    uint32_t start;
    uint32_t stop;
    double sumOfMaxComp;
#ifdef MAXCLL_PERCENTILE
    uint32_t *nitCounts;
#endif
    uint16_t maxNits;
    uint8_t bytesPerColor;
} ThreadData;

DWORD WINAPI ThreadFunc(LPVOID lpParam) {
    ThreadData *d = (ThreadData *) lpParam;
    uint8_t *pixels = d->pixels;
    uint8_t bytesPerColor = d->bytesPerColor;
    uint16_t *converted = d->converted;
    uint32_t width = d->width;
    uint32_t start = d->start;
    uint32_t stop = d->stop;

    float maxMaxComp = 0;
    double sumOfMaxComp = 0;

    for (uint32_t i = start; i < stop; i++) {
        for (uint32_t j = 0; j < width; j++) {
            float bt2020[3];

            if (bytesPerColor == 4) {
                matrixVectorMult((float *) pixels + i * 4 * width + 4 * j, bt2020, scrgb_to_bt2100);
            } else {
                float cur[3];
                _Float16 *cur16 = (_Float16 *) pixels + i * 4 * width + 4 * j;
                for (int k = 0; k < 3; k++) {
                    cur[k] = (float) cur16[k];
                }
                matrixVectorMult(cur, bt2020, scrgb_to_bt2100);
            }

            for (int k = 0; k < 3; k++) {
                bt2020[k] = saturate(bt2020[k]);
            }

            float maxComp = max(bt2020[0], max(bt2020[1], bt2020[2]));

#ifdef MAXCLL_PERCENTILE
            uint32_t nits = (uint32_t) roundf(maxComp * 10000);
            d->nitCounts[nits]++;
#endif
            if (maxComp > maxMaxComp) {
                maxMaxComp = maxComp;
            }

            sumOfMaxComp += maxComp;

            for (int k = 0; k < 3; k++) {
                float quant = roundf(pq_inv_eotf(bt2020[k]) * ((1 << TARGET_BITS) - 1));
                converted[(size_t) 3 * width * i + (size_t) 3 * j + k] = (uint16_t) roundf(
                        quant / ((1 << TARGET_BITS) - 1) * ((1 << INTERMEDIATE_BITS) - 1));
            }
        }
    }

    d->maxNits = (uint16_t) roundf(maxMaxComp * 10000);
    d->sumOfMaxComp = sumOfMaxComp;

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "jxr_to_png input.jxr [output.png]\n");
        return 1;
    }

    LPWSTR inputFile;
    LPWSTR outputFile = L"output.png";

    {
        LPWSTR *szArglist;
        int nArgs;

        szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (NULL == szArglist) {
            fprintf(stderr, "CommandLineToArgvW failed\n");
            return 1;
        }

        inputFile = szArglist[1];

        if (argc == 3) {
            outputFile = szArglist[2];
        }
    }

    // Create a decoder
    IWICBitmapDecoder *pDecoder = NULL;

    // Initialize COM
    CoInitialize(NULL);

    // The factory pointer
    IWICImagingFactory *pFactory = NULL;

    // Create the COM imaging factory
    HRESULT hr = CoCreateInstance(
            &CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            &IID_IWICImagingFactory,
            (void **) &pFactory);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create WIC imaging factory\n");
        return 1;
    }

    hr = pFactory->lpVtbl->CreateDecoderFromFilename(
            pFactory,
            inputFile,                       // Image to be decoded
            NULL,                            // Do not prefer a particular vendor
            GENERIC_READ,                    // Desired read access to the file
            WICDecodeMetadataCacheOnDemand,  // Cache metadata when needed
            &pDecoder                        // Pointer to the decoder
    );

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to open file\n");
        return 1;
    }

    // Retrieve the first frame of the image from the decoder
    IWICBitmapFrameDecode *pFrame = NULL;

    hr = pDecoder->lpVtbl->GetFrame(pDecoder, 0, &pFrame);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get frame\n");
        return 1;
    }

    IWICBitmapSource *pBitmapSource = NULL;

    hr = pFrame->lpVtbl->QueryInterface(pFrame, &IID_IWICBitmapSource, (void **) &pBitmapSource);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get IWICBitmapSource\n");
        return 1;
    }

    WICPixelFormatGUID pixelFormat;

    hr = pBitmapSource->lpVtbl->GetPixelFormat(pBitmapSource, &pixelFormat);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get pixel format\n");
        return 1;
    }

    uint8_t bytesPerColor;

    if (IsEqualGUID((void *) &pixelFormat, (void *) &GUID_WICPixelFormat128bppRGBAFloat)) {
        bytesPerColor = 4;
    } else if (IsEqualGUID((void *) &pixelFormat, (void *) &GUID_WICPixelFormat64bppRGBAHalf)) {
        bytesPerColor = 2;
    } else {
        fprintf(stderr, "Unsupported pixel format\n");
        return 1;
    }

    uint32_t width, height;

    hr = pBitmapSource->lpVtbl->GetSize(pBitmapSource, &width, &height);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get size\n");
        return 1;
    }

    size_t converted_size = sizeof(uint16_t) * width * height * 3;
    uint16_t *converted = malloc(converted_size);

    if (converted == NULL) {
        fprintf(stderr, "Failed to allocate converted pixels\n");
        return 1;
    }

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    uint32_t numThreads = systemInfo.dwNumberOfProcessors;
    printf("Using %d threads\n", numThreads);

    puts("Converting pixels to BT.2100 PQ...");

    uint16_t maxCLL, maxPALL;

    {

        UINT cbStride = width * bytesPerColor * 4;
        UINT cbBufferSize = cbStride * height;

        uint8_t *pixels = malloc(cbBufferSize);

        if (converted == NULL) {
            fprintf(stderr, "Failed to allocate float pixels\n");
            return 1;
        }

        WICRect rc;
        rc.Y = 0;
        rc.X = 0;
        rc.Width = (int) width;
        rc.Height = (int) height;
        hr = pBitmapSource->lpVtbl->CopyPixels(pBitmapSource,
                                               &rc,
                                               cbStride,
                                               cbBufferSize,
                                               pixels);

        if (FAILED(hr)) {
            fprintf(stderr, "Failed to copy pixels\n");
            exit(1);
        }

        uint32_t convThreads = min(numThreads, 64);

        uint32_t chunkSize = height / convThreads;

        if (chunkSize == 0) {
            convThreads = height;
            chunkSize = 1;
        }

        HANDLE hThreadArray[convThreads];
        ThreadData *threadData[convThreads];
        DWORD dwThreadIdArray[convThreads];

        for (uint32_t i = 0; i < convThreads; i++) {
            threadData[i] = malloc(sizeof(ThreadData));
            if (threadData[i] == NULL) {
                fprintf(stderr, "Failed to allocate thread data\n");
                return 1;
            }

            threadData[i]->pixels = pixels;
            threadData[i]->bytesPerColor = bytesPerColor;
            threadData[i]->converted = converted;
            threadData[i]->width = width;
            threadData[i]->start = i * chunkSize;
            if (i != convThreads - 1) {
                threadData[i]->stop = (i + 1) * chunkSize;
            } else {
                threadData[i]->stop = height;
            }

#ifdef MAXCLL_PERCENTILE
            threadData[i]->nitCounts = calloc(10000, sizeof(typeof(threadData[i]->nitCounts[0])));
#endif

            HANDLE hThread = CreateThread(
                    NULL,                   // default security attributes
                    0,                      // use default stack size
                    ThreadFunc,       // thread function name
                    threadData[i],          // argument to thread function
                    0,                      // use default creation flags
                    &dwThreadIdArray[i]);   // returns the thread identifier

            if (hThread) {
                hThreadArray[i] = hThread;
            } else {
                fprintf(stderr, "Failed to create thread\n");
                return 1;
            }
        }

        WaitForMultipleObjects(convThreads, hThreadArray, TRUE, INFINITE);

        maxCLL = 0;
        double sumOfMaxComp = 0;

        for (uint32_t i = 0; i < convThreads; i++) {
            HANDLE hThread = hThreadArray[i];

            DWORD exitCode;
            if (!GetExitCodeThread(hThread, &exitCode) || exitCode) {
                fprintf(stderr, "Thread failed to terminate properly\n");
                return 1;
            }
            CloseHandle(hThread);

            uint16_t tMaxNits = threadData[i]->maxNits;
            if (tMaxNits > maxCLL) {
                maxCLL = tMaxNits;
            }

            sumOfMaxComp += threadData[i]->sumOfMaxComp;
        }

#ifdef MAXCLL_PERCENTILE
        uint16_t currentIdx = maxCLL;
        uint64_t count = 0;
        uint64_t countTarget = (uint64_t) round((1 - MAXCLL_PERCENTILE) * (double) ((uint64_t) width * height));
        while (1) {
            for (uint32_t i = 0; i < convThreads; i++) {
                count += threadData[i]->nitCounts[currentIdx];
            }
            if (count >= countTarget) {
                maxCLL = currentIdx;
                break;
            }
            currentIdx--;
        }
#endif

        for (uint32_t i = 0; i < convThreads; i++) {
#ifdef MAXCLL_PERCENTILE
            free(threadData[i]->nitCounts);
#endif
            free(threadData[i]);
        }

        maxPALL = (uint16_t) round(10000 * (sumOfMaxComp / (double) ((uint64_t) width * height)));

        free(pixels);
    }

    printf("Computed HDR metadata: %u MaxCLL, %u MaxPALL\n", maxCLL, maxPALL);

    FILE *f = _wfopen(outputFile, L"wb");

    if (!f) {
        perror("Error opening file");
        return 1;
    }

    spng_ctx *enc = spng_ctx_new(SPNG_CTX_ENCODER);

    spng_set_png_file(enc, f);

    struct spng_ihdr ihdr =
            {
                    .width = width,
                    .height = height,
                    .bit_depth = INTERMEDIATE_BITS,
                    .color_type = SPNG_COLOR_TYPE_TRUECOLOR
            };

    spng_set_ihdr(enc, &ihdr);

    unsigned char cicp_data[4] =
            {
                    9, // Colour Primaries (BT.2020)
                    16, // Transfer Function (PQ)
                    0, // Matrix Coefficients, always 0
                    1  // The Video Full Range Flag value MUST be either 0 or 1.
            };

    unsigned char clli_data[8];

    uint32_t maxCLL_png = maxCLL * 10000;
    uint32_t maxFALL_png = maxPALL * 10000;

    clli_data[0] = (maxCLL_png >> 24) & 0xFF;
    clli_data[1] = (maxCLL_png >> 16) & 0xFF;
    clli_data[2] = (maxCLL_png >> 8) & 0xFF;
    clli_data[3] = maxCLL_png & 0xFF;

    clli_data[4] = (maxFALL_png >> 24) & 0xFF;
    clli_data[5] = (maxFALL_png >> 16) & 0xFF;
    clli_data[6] = (maxFALL_png >> 8) & 0xFF;
    clli_data[7] = maxFALL_png & 0xFF;

    struct spng_unknown_chunk chunks[2] = {
            {
                    // cICP
                    .type = {0x63, 0x49, 0x43, 0x50},
                    .length = 4,
                    .data = cicp_data,
                    .location = SPNG_AFTER_IHDR, // Also means before PLTE
            },
            {
                    // cLLi
                    .type = {0x63, 0x4C, 0x4C, 0x69},
                    .length = 8,
                    .data = clli_data,
                    .location = SPNG_AFTER_IHDR, // Also means before PLTE
            }};

    spng_set_unknown_chunks(enc, chunks, 2);

    struct spng_iccp iccp = {
            .profile = icc_data,
            .profile_len = sizeof(icc_data)
    };

    strcpy(iccp.profile_name, icc_name);

    spng_set_iccp(enc, &iccp);

    printf("Doing PNG encoding...\n");
    int err = spng_encode_image(enc, converted, converted_size, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);

    if (err) {
        printf("Got error code %d from libspng\n", err);
        return 1;
    }
    printf("Encode success: %ld total bytes\n", ftell(f));
}
