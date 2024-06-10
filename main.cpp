#define _CRT_SECURE_NO_WARNINGS
#define _XM_F16C_INTRINSICS_

#include <cstdio>
#include <cstring>
#include <windows.h>
#include <wincodec.h>
#include <cmath>
#include "DirectXMath/DirectXMath.h"
#include "DirectXMath/DirectXPackedVector.h"
#include "libpng/png.h"
#include "icc_profile.h"

using namespace DirectX;
using namespace DirectX::PackedVector;

#define INTERMEDIATE_BITS 16  // PNG bit depth (can only be 8 or 16, and 8 is insufficient for HDR)
#define TARGET_BITS 10  // quantization bit depth

#define MAXCLL_PERCENTILE 0.9999  // comment out to calculate true MaxCLL instead of top percentile


static const XMVECTOR vm1 = XMVectorReplicate(1305.0f / 8192.0f);
static const XMVECTOR vm2 = XMVectorReplicate(2523.0f / 32.0f);
static const XMVECTOR vc1 = XMVectorReplicate(107.0f / 128.0f);
static const XMVECTOR vc2 = XMVectorReplicate(2413.0f / 128.0f);
static const XMVECTOR vc3 = XMVectorReplicate(2392.0f / 128.0f);

inline XMVECTOR pq_inv_eotf(XMVECTOR y) {
    XMVECTOR pow1 = XMVectorPow(y, vm1);
    return XMVectorPow(
            XMVectorDivide(
                    XMVectorAdd(vc1, XMVectorMultiply(vc2, pow1)),
                    XMVectorAdd(g_XMOne, XMVectorMultiply(vc3, pow1))),
            vm2);
}

static const XMMATRIX scrgb_to_bt2100 = {
        {2939026994.L / 585553224375.L,  76515593.L / 138420033750.L,   12225392.L / 93230009375.L,      0},
        {9255011753.L / 3513319346250.L, 6109575001.L / 830520202500.L, 1772384008.L / 2517210253125.L,  0},
        {173911579.L / 501902763750.L,   75493061.L / 830520202500.L,   18035212433.L / 2517210253125.L, 0},
        {0,                              0,                             0,                               1}};

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
    auto d = (ThreadData *) lpParam;
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
            XMVECTOR v;

            if (bytesPerColor == 4) {
                v = XMLoadFloat4A((XMFLOAT4A *) ((FLOAT *) pixels + i * 4 * width + 4 * j));
            } else {
                v = XMLoadHalf4((XMHALF4 * )((HALF *) pixels + i * 4 * width + 4 * j));
            }

            v = XMVectorSaturate(XMVector3Transform(v, scrgb_to_bt2100));

            auto bt2020 = XMFLOAT4A();

            XMStoreFloat4A(&bt2020, v);

            float maxComp = max(bt2020.x, max(bt2020.y, bt2020.z));

#ifdef MAXCLL_PERCENTILE
            auto nits = (uint32_t) roundf(maxComp * 10000);
            d->nitCounts[nits]++;
#endif
            if (maxComp > maxMaxComp) {
                maxMaxComp = maxComp;
            }

            sumOfMaxComp += maxComp;

            const auto maxTarget = (float) ((1 << TARGET_BITS) - 1);
            const auto maxIntermediate = (float) ((1 << INTERMEDIATE_BITS) - 1);

            __m128i vint = _mm_cvtps_epi32(
                    XMVectorMultiply(
                            _mm_round_ps(
                                    XMVectorMultiply(
                                            pq_inv_eotf(v),
                                            XMVectorReplicate(maxTarget)),
                                    _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC),
                            XMVectorReplicate(maxIntermediate / maxTarget)));

            __m128i vshort = _mm_packus_epi32(vint, vint);

            const __m128i reverse_endian_mask = _mm_set_epi8(
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 4, 5, 2, 3, 0, 1);
            vshort = _mm_shuffle_epi8(vshort, reverse_endian_mask);

            uint16_t *dst = &converted[(size_t) 3 * width * i + (size_t) 3 * j];

            uint16_t result[4];
            _mm_storel_epi64((__m128i *) result, vshort);
            memcpy(dst, result, 3 * sizeof(uint16_t));
        }
    }

    d->maxNits = (uint16_t) roundf(maxMaxComp * 10000);
    d->sumOfMaxComp = sumOfMaxComp;

    return 0;
}

int write_png_file(FILE *file, png_bytep data, uint32_t width, uint32_t height, uint32_t maxCLL, uint32_t maxFALL) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        fprintf(stderr, "Could not create PNG write struct\n");
        return 1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        fprintf(stderr, "Could not create PNG info struct\n");
        return 1;
    }

    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "Error during PNG creation\n");
        return 1;
    }

    png_init_io(png, file);

    png_set_IHDR(
            png, info, width, height, 16, PNG_COLOR_TYPE_RGB,
            PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
    );

    uint8_t cicp_data[4] = {9, 16, 0, 1};

    uint8_t clli_data[8] = {
            (uint8_t) ((maxCLL >> 24) & 0xFF),
            (uint8_t) ((maxCLL >> 16) & 0xFF),
            (uint8_t) ((maxCLL >> 8) & 0xFF),
            (uint8_t) (maxCLL & 0xFF),
            (uint8_t) ((maxFALL >> 24) & 0xFF),
            (uint8_t) ((maxFALL >> 16) & 0xFF),
            (uint8_t) ((maxFALL >> 8) & 0xFF),
            (uint8_t) (maxFALL & 0xFF),
    };

    png_unknown_chunk unknown_chunks[] = {
            {.name = {'c', 'I', 'C', 'P'}, .data = cicp_data, .size = 4, .location = PNG_HAVE_IHDR},
            {.name = {'c', 'L', 'L', 'i'}, .data = clli_data, .size = 8, .location = PNG_HAVE_IHDR}
    };

    int num_unknowns = sizeof(unknown_chunks) / sizeof(unknown_chunks[0]);

    for (int i = 0; i < num_unknowns; i++) {
        png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_ALWAYS, unknown_chunks[i].name, 1);
        png_set_unknown_chunks(png, info, &unknown_chunks[i], 1);
    }

    png_set_iCCP(png, info, icc_name, PNG_COMPRESSION_TYPE_BASE, icc_data, sizeof(icc_data));

    png_color_8 sig_bit = {.red = TARGET_BITS, .green = TARGET_BITS, .blue = TARGET_BITS};
    png_set_sBIT(png, info, &sig_bit);

    png_write_info(png, info);

    auto *row_pointers = (png_bytep *) malloc(sizeof(png_bytep) * height);
    if (row_pointers == nullptr) {
        fprintf(stderr, "Failed to allocate PNG row pointers\n");
        return 1;
    }

    for (uint32_t y = 0; y < height; y++) {
        row_pointers[y] = data + (size_t) y * width * 6; // 6 bytes per pixel for RGB16
    }

    png_write_image(png, row_pointers);
    png_write_end(png, nullptr);

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "jxr_to_png input.jxr [output.png]\n");
        return 1;
    }

    LPWSTR inputFile;
    auto outputFile = (LPWSTR) L"output.png";

    {
        LPWSTR *szArglist;
        int nArgs;

        szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (nullptr == szArglist) {
            fprintf(stderr, "CommandLineToArgvW failed\n");
            return 1;
        }

        inputFile = szArglist[1];

        if (argc == 3) {
            outputFile = szArglist[2];
        }
    }

    // Create a decoder
    IWICBitmapDecoder *pDecoder = nullptr;

    // Initialize COM
    CoInitialize(nullptr);

    // The factory pointer
    IWICImagingFactory *pFactory = nullptr;

    // Create the COM imaging factory
    HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory,
            (void **) &pFactory);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create WIC imaging factory\n");
        return 1;
    }

    hr = pFactory->CreateDecoderFromFilename(
            inputFile,                       // Image to be decoded
            nullptr,                            // Do not prefer a particular vendor
            GENERIC_READ,                    // Desired read access to the file
            WICDecodeMetadataCacheOnDemand,  // Cache metadata when needed
            &pDecoder                        // Pointer to the decoder
    );

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to open input file\n");
        return 1;
    }

    // Retrieve the first frame of the image from the decoder
    IWICBitmapFrameDecode *pFrame = nullptr;

    hr = pDecoder->GetFrame(0, &pFrame);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get frame\n");
        return 1;
    }

    IWICBitmapSource *pBitmapSource = nullptr;

    hr = pFrame->QueryInterface(IID_IWICBitmapSource, (void **) &pBitmapSource);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get IWICBitmapSource\n");
        return 1;
    }

    WICPixelFormatGUID pixelFormat;

    hr = pBitmapSource->GetPixelFormat(&pixelFormat);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get pixel format\n");
        return 1;
    }

    uint8_t bytesPerColor;

    if (IsEqualGUID(pixelFormat, GUID_WICPixelFormat128bppRGBAFloat)) {
        bytesPerColor = 4;
    } else if (IsEqualGUID(pixelFormat, GUID_WICPixelFormat64bppRGBAHalf)) {
        bytesPerColor = 2;
    } else {
        fprintf(stderr, "Unsupported pixel format\n");
        return 1;
    }

    uint32_t width, height;

    hr = pBitmapSource->GetSize(&width, &height);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get size\n");
        return 1;
    }

    size_t converted_size = sizeof(uint16_t) * width * height * 3;
    auto converted = (uint16_t *) malloc(converted_size);

    if (converted == nullptr) {
        fprintf(stderr, "Failed to allocate converted pixels\n");
        return 1;
    }

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    uint32_t numThreads = min(8, systemInfo.dwNumberOfProcessors / 2);
    printf("Using %d threads\n", numThreads);

    puts("Converting pixels to BT.2100 PQ...");

    uint16_t maxCLL, maxPALL;

    {

        UINT cbStride = width * bytesPerColor * 4;
        UINT cbBufferSize = cbStride * height;

        auto pixels = (uint8_t *) malloc(cbBufferSize);

        if (pixels == nullptr) {
            fprintf(stderr, "Failed to allocate float pixels\n");
            return 1;
        }

        WICRect rc;
        rc.Y = 0;
        rc.X = 0;
        rc.Width = (int) width;
        rc.Height = (int) height;
        hr = pBitmapSource->CopyPixels(
                &rc,
                cbStride,
                cbBufferSize,
                pixels);

        if (FAILED(hr)) {
            fprintf(stderr, "Failed to copy pixels\n");
            exit(1);
        }

        uint32_t convThreads = numThreads;

        uint32_t chunkSize = height / convThreads;

        if (chunkSize == 0) {
            convThreads = height;
            chunkSize = 1;
        }

        auto hThreadArray = (HANDLE *) malloc(sizeof(HANDLE) * convThreads);
        auto threadData = (ThreadData **) malloc(sizeof(ThreadData *) * convThreads);
        auto dwThreadIdArray = (DWORD *) malloc(sizeof(DWORD) * convThreads);

        if (hThreadArray == nullptr || threadData == nullptr || dwThreadIdArray == nullptr) {
            fprintf(stderr, "Failed to allocate array for thread data\n");
            return 1;
        }

        for (uint32_t i = 0; i < convThreads; i++) {
            threadData[i] = (ThreadData *) malloc(sizeof(ThreadData));
            if (threadData[i] == nullptr) {
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
            threadData[i]->nitCounts = (uint32_t *) calloc(10000, sizeof(uint32_t));
#endif

            HANDLE hThread = CreateThread(
                    nullptr,                   // default security attributes
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
        auto countTarget = (uint64_t) round((1 - MAXCLL_PERCENTILE) * (double) ((uint64_t) width * height));
        while (true) {
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

        free(hThreadArray);
        free(threadData);
        free(dwThreadIdArray);

        maxPALL = (uint16_t) round(10000 * (sumOfMaxComp / (double) ((uint64_t) width * height)));

        free(pixels);
    }

    printf("Computed HDR metadata: %u MaxCLL, %u MaxFALL\n", maxCLL, maxPALL);

    FILE *f = _wfopen(outputFile, L"wb");

    if (!f) {
        perror("Error opening output file");
        return 1;
    }

    uint32_t maxCLL_png = maxCLL * 10000;
    uint32_t maxFALL_png = maxPALL * 10000;

    printf("Doing PNG encoding...\n");
    if (write_png_file(f, (unsigned char *) converted, width, height, maxCLL_png, maxFALL_png)) {
        printf("Error on PNG encode\n");
        return 1;
    }

    printf("Encode success: %ld total bytes\n", ftell(f));
}
