################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../LIBJPEG/Target/jdata_conf.c 

OBJS += \
./LIBJPEG/Target/jdata_conf.o 

C_DEPS += \
./LIBJPEG/Target/jdata_conf.d 


# Each subdirectory must supply rules for building sources it contributes
LIBJPEG/Target/%.o LIBJPEG/Target/%.su LIBJPEG/Target/%.cyclo: ../LIBJPEG/Target/%.c LIBJPEG/Target/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DUSE_FULL_LL_DRIVER -DSTM32F767xx -DUSE_HAL_DRIVER -c -I../Core/Inc -I../Drivers/STM32F7xx_HAL_Driver/Inc -I../Drivers/CMSIS/Device/ST/STM32F7xx/Include -I../Drivers/CMSIS/Include -I../Drivers/STM32F7xx_HAL_Driver/Inc/Legacy -I../FATFS/Target -I../FATFS/App -I../LIBJPEG/App -I../LIBJPEG/Target -I../Middlewares/Third_Party/FatFs/src -I../Middlewares/Third_Party/LibJPEG/include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -ftls-model=local-exec -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-LIBJPEG-2f-Target

clean-LIBJPEG-2f-Target:
	-$(RM) ./LIBJPEG/Target/jdata_conf.cyclo ./LIBJPEG/Target/jdata_conf.d ./LIBJPEG/Target/jdata_conf.o ./LIBJPEG/Target/jdata_conf.su

.PHONY: clean-LIBJPEG-2f-Target

