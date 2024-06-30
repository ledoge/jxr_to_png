## [Download latest release](https://github.com/ledoge/jxr_to_png/releases/latest/download/release.zip)
([Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) required, which you likely already have installed)

# About
This is a simple command line tool for converting HDR JPEG XR files, such as Windows HDR screenshots, to PNG.

The output format is 16 bit PNG with BT.2100 + PQ color space, but the actual data is quantized to 10 bits to try to keep the size reasonably low. The files should display properly in any Chromium-based browser, which includes Electron apps like the desktop version of Discord.

# Usage
```
jxr_to_png input.jxr [output.png]
```

Instead of using the command line, you can also drag a .jxr file onto the executable.

# HDR metadata
The MaxCLL value is calculated as suggested in the paper [On the Calculation and Usage of HDR Static Content Metadata](https://doi.org/10.5594/JMI.2021.3090176), by taking the light level of the 99.99 percentile brightest pixel. This is an underestimate of the "real" MaxCLL value calculated according to H.274, so it technically causes some clipping when tone mapping. However, following the spec can lead to a much higher MaxCLL value, which causes e.g. Chromium's tone mapping to significantly dim the entire image, so this trade-off seems to be worth it.
