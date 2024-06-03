## [Download latest release](https://github.com/ledoge/jxr_to_png/releases/latest/download/release.zip)
([Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) required, which you likely already have installed)

# About
This is a simple command line tool for converting HDR JPEG-XR files, such as Windows HDR screenshots, to PNG.

The output format is 16 bit PNG with BT.2100 + PQ color space, but the actual data is quantized to 10 bits to try to keep the size reasonably low.

# Usage
```
jxr_to_png input.jxr [output.png]
```

# HDR metadata
The MaxCLL value is calculated almost identically to [HDR + WCG Image Viewer](https://github.com/13thsymphony/HDRImageViewer) by taking the light level of the 99.99 percentile brightest pixel. This is an underestimate of the "real" MaxCLL value calculated according to H.274, so it technically causes some clipping when tone mapping. However, following the spec can lead to a much higher MaxCLL value, which causes e.g. Chromium's tone mapping to significantly dim the entire image, so this trade-off seems to be worth it.
