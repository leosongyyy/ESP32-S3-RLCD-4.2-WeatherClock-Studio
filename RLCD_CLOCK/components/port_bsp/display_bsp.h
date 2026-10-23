// 声明 RLCD 显示屏尺寸、颜色和刷新接口。
#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>


#ifndef AlgorithmOptimization
#define AlgorithmOptimization  3     //1:原始算法 2:采用移位算法 3:查表法   来优化CPU
#endif

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

void Display_SetOtaQuietMode(bool enabled);
void Display_AcquireDmaConservativeMode();
void Display_ReleaseDmaConservativeMode();

class DisplayPort {
  private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    spi_host_device_t   spihost_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen = 0;
    bool                spi_bus_initialized_ = false;
    bool                reset_gpio_configured_ = false;
    bool                ready_ = false;
    bool                initializing_ = false;
#if (AlgorithmOptimization == 3)
    uint16_t *PixelIndexLUT = NULL;
    uint8_t *PixelBitLUT = NULL;
    size_t PixelLutOffset(uint16_t x, uint16_t y) const;
    void InitPortraitLUT();
    void InitLandscapeLUT();
#endif

    void ReleaseResources();
    void Set_ResetIOLevel(uint8_t level);
    bool RLCD_SendParamChecked(int command, const void *data, size_t data_size, const char *kind, uint8_t value);
    bool RLCD_SendCommand(uint8_t Reg);
    bool RLCD_SendData(uint8_t Data);
    bool RLCD_WaitForPendingColorTransfers();
    bool RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    void KeepPinsActiveInLightSleep(void);

  public:
    DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost = SPI3_HOST);
    ~DisplayPort();
    DisplayPort(const DisplayPort &) = delete;
    DisplayPort &operator=(const DisplayPort &) = delete;
    bool IsReady() const;
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
    void RLCD_DisplayXRange(uint16_t x1, uint16_t x2);

    // 直接帧缓冲访问接口：供非 LVGL 的整屏绘制通路（如 Game Boy 模拟器）使用。
    // 返回内部打包帧缓冲的起始地址；显示尚未就绪时返回 nullptr。
    // 写入语义与 RLCD_SetPixel 完全一致：bit=1 为白，bit=0 为黑。
    uint8_t *RLCD_Framebuffer();
    // 返回帧缓冲字节数（400x300 时为 15000）。
    int RLCD_FramebufferSize();
    // 把整块帧缓冲推屏，等价于 RLCD_Display()。
    void RLCD_Present();
    // 以面板列（byte_x，0..width/2-1）为单位填充连续竖向条带。
    // 每个面板字节覆盖 2 列 x 4 行，因此该区间在帧缓冲内是连续内存。
    bool RLCD_FillRange(uint16_t byte_x1, uint16_t byte_x2, uint8_t value);
	#if (AlgorithmOptimization != 3)
    void RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color);      //竖屏显示
    void RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color);     //横屏显示
	#endif
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
};
