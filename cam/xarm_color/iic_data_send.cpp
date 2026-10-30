#include <string.h>
#include "iic_data_send.hpp"
#include "Wire.h"
#include "colors.h"
#include "muffin_link.h"

#define I2C_SLAVE_ADDRESS 0x52

static QueueHandle_t xQueueResultI = NULL;
static QueueHandle_t xQueueResultO = NULL;

static const char *TAG = "iic_data_send";
static const int sdaPin = 47;
static const int sclPin = 48;
static const uint32_t i2cFrequency = 100000;

static send_color_data_t color_data[5];

static uint8_t rec = 0xFF;
static uint8_t send_data[4] = {0};

static void iic_receive(int len)
{
  while(Wire.available())
  {
    rec = Wire.read();
  }  
}

// xarm: reg 0x00/0x01 = the two configured colors, 0x10..0x14 = any of the five. 4 bytes: x, y, w, h
static void iic_request()
{
  int c = rec == 0x00 ? SLOT0_COLOR : rec == 0x01 ? SLOT1_COLOR
        : (rec >= 0x10 && rec <= 0x14) ? rec - 0x10 : -1;
  if (c >= 0) {
    send_data[0] = color_data[c].center_x;
    send_data[1] = color_data[c].center_y;
    send_data[2] = color_data[c].width;
    send_data[3] = color_data[c].length;
  } else {
    memset(send_data, 0, sizeof send_data);
  }
  Wire.slaveWrite(send_data, sizeof(send_data));
}

static void task_process_handler(void *arg)
{
  /* IIC初始化 */
  Wire.begin((uint8_t)I2C_SLAVE_ADDRESS, sdaPin, sclPin, i2cFrequency);
  /* 注册接收数据的回调函数 */
  Wire.onReceive(iic_receive);
  /* 注册请求数据的回调函数 */
  Wire.onRequest(iic_request);
  /* xarm: wireless too — join Muffin's WiFi and send each frame over UDP (muffin_link.cpp) */
  link_begin();

  while (true)
  {

    if (xQueueReceive(xQueueResultI, &color_data, portMAX_DELAY))
    {
      link_send(color_data);
    //  switch(rec){
    //   case 0x00:
    //     printf("red:%d",rec);
    //     break;
    //   case 0x01:
    //     printf("blue:%d",rec);
    //     break;
    //  }
    }
  }
}

void register_iic_data_send(const QueueHandle_t result_i,
                            const QueueHandle_t result_o)
{
  xQueueResultI = result_i;
  xQueueResultO = result_o;

  xTaskCreatePinnedToCore(task_process_handler, TAG, 4 * 1024, NULL, 5, NULL, 1);
}