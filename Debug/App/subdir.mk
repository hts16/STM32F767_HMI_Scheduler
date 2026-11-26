################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../App/App_JPEG.c \
../App/App_LCD.c \
../App/App_LED.c \
../App/App_SDCARD.c \
../App/App_SDRAM.c \
../App/App_TOUCH.c \
../App/App_font8x16.c \
../App/App_main.c 

OBJS += \
./App/App_JPEG.o \
./App/App_LCD.o \
./App/App_LED.o \
./App/App_SDCARD.o \
./App/App_SDRAM.o \
./App/App_TOUCH.o \
./App/App_font8x16.o \
./App/App_main.o 

C_DEPS += \
./App/App_JPEG.d \
./App/App_LCD.d \
./App/App_LED.d \
./App/App_SDCARD.d \
./App/App_SDRAM.d \
./App/App_TOUCH.d \
./App/App_font8x16.d \
./App/App_main.d 


# Each subdirectory must supply rules for building sources it contributes
App/%.o App/%.su App/%.cyclo: ../App/%.c App/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DUSE_FULL_LL_DRIVER -DSTM32F767xx -DUSE_HAL_DRIVER -c -I../Core/Inc -I../Drivers/STM32F7xx_HAL_Driver/Inc -I../Drivers/CMSIS/Device/ST/STM32F7xx/Include -I../Drivers/CMSIS/Include -I../Drivers/STM32F7xx_HAL_Driver/Inc/Legacy -I../FATFS/Target -I../FATFS/App -I../LIBJPEG/App -I../LIBJPEG/Target -I../Middlewares/Third_Party/FatFs/src -I../Middlewares/Third_Party/LibJPEG/include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -ftls-model=local-exec -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-App

clean-App:
	-$(RM) ./App/App_JPEG.cyclo ./App/App_JPEG.d ./App/App_JPEG.o ./App/App_JPEG.su ./App/App_LCD.cyclo ./App/App_LCD.d ./App/App_LCD.o ./App/App_LCD.su ./App/App_LED.cyclo ./App/App_LED.d ./App/App_LED.o ./App/App_LED.su ./App/App_SDCARD.cyclo ./App/App_SDCARD.d ./App/App_SDCARD.o ./App/App_SDCARD.su ./App/App_SDRAM.cyclo ./App/App_SDRAM.d ./App/App_SDRAM.o ./App/App_SDRAM.su ./App/App_TOUCH.cyclo ./App/App_TOUCH.d ./App/App_TOUCH.o ./App/App_TOUCH.su ./App/App_font8x16.cyclo ./App/App_font8x16.d ./App/App_font8x16.o ./App/App_font8x16.su ./App/App_main.cyclo ./App/App_main.d ./App/App_main.o ./App/App_main.su

.PHONY: clean-App

