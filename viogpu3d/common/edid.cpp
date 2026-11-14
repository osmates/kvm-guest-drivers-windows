#include "edid.h"

// clang-format off
static VIC_MODE gpu_vic_mode_table[] =
{
    {1, {640, 480}},
    {2, {720, 480}},
    {3, {720, 480}},
    {4, {1280, 720}},
    {5, {1920, 1080}},
    {6, {720, 480}},
    {7, {720, 480}},
    {8, {720, 240}},
    {9, {720, 240}},

    {10, {2880, 480}},
    {11, {2880, 480}},
    {12, {2880, 240}},
    {13, {2880, 240}},
    {14, {1440, 480}},
    {15, {1440, 480}},
    {16, {1920, 1080}},
    {17, {720, 576}},
    {18, {720, 576}},
    {19, {1280, 720}},

    {20, {1920, 1080}},
    {21, {720, 576}},
    {22, {720, 576}},
    {23, {720, 288}},
    {24, {720, 288}},
    {25, {2880, 576}},
    {26, {2880, 576}},
    {27, {2880, 288}},
    {28, {2880, 288}},
    {29, {1440, 576}},

    {30, {1440, 576}},
    {31, {1920, 1080}},
    {32, {1920, 1080}},
    {33, {1920, 1080}},
    {34, {1920, 1080}},
    {35, {2880, 480}},
    {36, {2880, 480}},
    {37, {2880, 576}},
    {38, {2880, 576}},
    {39, {1920, 1080}},

    {40, {1920, 1080}},
    {41, {1280, 720}},
    {42, {720, 576}},
    {43, {720, 576}},
    {44, {720, 576}},
    {45, {720, 576}},
    {46, {1920, 1080}},
    {47, {1280, 720}},
    {48, {720, 480}},
    {49, {720, 480}},

    {50, {720, 480}},
    {51, {720, 480}},
    {52, {720, 576}},
    {53, {720, 576}},
    {54, {720, 576}},
    {55, {720, 576}},
    {56, {720, 480}},
    {57, {720, 480}},
    {58, {720, 480}},
    {59, {720, 480}},

    {60, {1280, 720}},
    {61, {1280, 720}},
    {62, {1280, 720}},
    {63, {1920, 1080}},
    {64, {1920, 1080}},
    {65, {1280, 720}},
    {66, {1280, 720}},
    {67, {1280, 720}},
    {68, {1280, 720}},
    {69, {1280, 720}},

    {70, {1280, 720}},
    {71, {1280, 720}},
    {72, {1920, 1080}},
    {73, {1920, 1080}},
    {74, {1920, 1080}},
    {75, {1920, 1080}},
    {76, {1920, 1080}},
    {77, {1920, 1080}},
    {78, {1920, 1080}},
    {79, {1680, 720}},

    {80, {1680, 720}},
    {81, {1680, 720}},
    {82, {1680, 720}},
    {83, {1680, 720}},
    {84, {1680, 720}},
    {85, {1680, 720}},
    {86, {2560, 1080}},
    {87, {2560, 1080}},
    {88, {2560, 1080}},
    {89, {2560, 1080}},

    {90, {2560, 1080}},
    {91, {2560, 1080}},
    {92, {2560, 1080}},
    {93, {3840, 2160}},
    {94, {3840, 2160}},
    {95, {3840, 2160}},
    {96, {3840, 2160}},
    {97, {3840, 2160}},
    {98, {4096, 2160}},
    {99, {4096, 2160}},

    {100, {4096, 2160}},
    {101, {4096, 2160} },
    {102, {4096, 2160} },
    {103, {3840, 2160} },
    {104, {3840, 2160} },
    {105, {3840, 2160} },
    {106, {3840, 2160} },
    {107, {3840, 2160} },
    {108, {1280, 720} },
    {109, {1280, 720} },

    {110, {1680, 720} },
    {111, {1920, 1080} },
    {112, {1920, 1080} },
    {113, {2560, 1080} },
    {114, {3840, 2160} },
    {115, {4096, 2160} },
    {116, {3840, 2160} },
    {117, {3840, 2160} },
    {118, {3840, 2160} },
    {119, {3840, 2160} },
    {120, {3840, 2160} },
    {121, {5120, 2160} },
    {122, {5120, 2160} },
    {123, {5120, 2160} },
    {124, {5120, 2160} },
    {125, {5120, 2160} },
    {126, {5120, 2160} },
    {127, {5120, 2160} },

    {193, {5120, 2160} },
    {194, {7680, 4320} },
    {195, {7680, 4320} },
    {196, {7680, 4320} },
    {197, {7680, 4320} },
    {198, {7680, 4320} },
    {199, {7680, 4320} },
    {200, {7680, 4320} },
    {201, {7680, 4320} },
    {202, {7680, 4320} },
    {203, {7680, 4320} },
    {204, {7680, 4320} },
    {205, {7680, 4320} },
    {206, {7680, 4320} },
    {207, {7680, 4320} },
    {208, {7680, 4320} },
    {209, {7680, 4320} },
    {210, {10240, 4320} },
    {211, {10240, 4320} },
    {212, {10240, 4320} },
    {213, {10240, 4320} },
    {214, {10240, 4320} },
    {215, {10240, 4320} },
    {216, {10240, 4320} },
    {217, {10240, 4320} },
    {218, {4096, 2160} },
    {219, {4096, 2160} },
};

VIOGPU_DISP_MODE gpu_disp_modes[64] = {0};

UCHAR g_gpu_edid[EDID_V1_BLOCK_SIZE] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF ,0xFF, 0x00, // Header
    0x49, 0x14,                                     // Manufacturef Id
    0x34, 0x12,                                     // Manufacturef product code
    0x00, 0x00, 0x00, 0x00,                         // serial number
    0xff, 0x1d,                                     // year of manufacture
    0x01,                                           // EDID version
    0x04,                                           // EDID revision
    0xa3,                                           // VideoInputDefinition digital, 8-bit, HDMI
    0x00,                                           //MaximumHorizontalImageSize
    0x00,                                           //MaximumVerticallImageSize
    0x78,                                           //DisplayTransferCharacteristics
    0x22,                                           //FeatureSupport
    0xEE, 0x95, 0xA3, 0x54, 0x4C,                   //ColorCharacteristics
    0x99, 0x26, 0x0F, 0x50, 0x54,
    0x00, 0x00,                                     //EstablishedTimings
    0x00,                                           //ManufacturerTimings
    0x01, 0x01,                                     //StandardTimings[8]
    0x01, 0x01,
    0x01, 0x01,
    0x01, 0x01,
    0x01, 0x01,
    0x01, 0x01,
    0x01, 0x01,
    0x01, 0x01,
    0x6c, 0x20, 0x80, 0x30, 0x42, 0x00,             // Descriptor 1
    0x32, 0x30, 0x40, 0xc0, 0x13, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
    0x00, 0x00, 0x00, 0xFD, 0x00, 0x32,             // Descriptor 2
    0x7d, 0x1e, 0xa0, 0x78, 0x01, 0x0a,
    0x20, 0x20 ,0x20, 0x20, 0x20, 0x20,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00,             // Descriptor 3
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00,             // Descriptor 4
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,                                           // Number of Extentions
    0x00                                            // CheckSum
};
// clang-format on
bool GetStandardTimingResolution(PSTANDARD_TIMING_DESCRIPTOR desc, PVIOGPU_DISP_MODE mode)
{
    if (desc && mode && (desc->HorizontalActivePixels > 0))
    {
        mode->XResolution = (desc->HorizontalActivePixels + 31) * 8;
        switch (desc->ImageAspectRatio)
        {
            case AR_16_10:
                mode->YResolution = ((mode->XResolution) / 16 * 10);
                break;
            case AR_4_3:
                mode->YResolution = ((mode->XResolution) / 4 * 3);
                break;
            case AR_5_4:
                mode->YResolution = ((mode->XResolution) / 5 * 4);
                break;
            case AR_16_9:
                mode->YResolution = ((mode->XResolution) / 16 * 9);
                break;
        }
        return true;
    }
    return false;
}

bool GetVICResolution(UCHAR svd, PVIOGPU_DISP_MODE mode)
{
    // A short video descriptor of 129 to 192 is VIC 1 to 64 with its native flag; 0, 128 and 254 to 255 are reserved
    // (CTA-861-G, 7.5.1).
    UCHAR vic = (svd >= 129 && svd <= 192) ? (UCHAR)(svd & 0x7F) : svd;
    if (mode == NULL || vic == 0)
    {
        return false;
    }
    for (ULONG i = 0; i < ARRAYSIZE(gpu_vic_mode_table); i++)
    {
        if (gpu_vic_mode_table[i].Index == vic)
        {
            *mode = gpu_vic_mode_table[i].Resolution;
            return true;
        }
    }
    return false;
}

bool GetDetailedTimingResolution(PEDID_DETAILED_DESCRIPTOR desc, PVIOGPU_DISP_MODE mode)
{
    // A pixel clock of 0 marks a display descriptor, not a timing (E-EDID 1.4, 3.10.2).
    if (desc == NULL || mode == NULL || desc->PixelClock == 0)
    {
        return false;
    }
    USHORT xres = (USHORT)(desc->HorizontalActiveLow | (desc->horizontalActiveHigh << 8));
    USHORT yres = (USHORT)(desc->VerticalActiveLow | (desc->VerticalActiveHigh << 8));
    // An interlaced timing gives the lines of one field.
    if (desc->Interlaced)
    {
        yres = (USHORT)(yres * 2);
    }
    if (xres == 0 || yres == 0)
    {
        return false;
    }
    mode->XResolution = xres;
    mode->YResolution = yres;
    return true;
}
