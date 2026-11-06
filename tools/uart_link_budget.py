#!/usr/bin/env python3
"""Nominal divider calculation from this board's HAL/IDF configuration; no hardware IO."""
import argparse
import json


def budget(baud=921600,application_bytes_per_second=11440.433373078635,data2_bytes_per_second=1350.):
    # STM32F405 HSE8MHz /8 *336 /2 /APB1div4 =42MHz.
    # UART_BRR_SAMPLING16 uses nearest integer peripheral-clock divisor.
    pclk=42000000
    centi_div=(pclk*25)//(4*baud);mant=centi_div//100
    hal_div=(mant<<4)+((centi_div-mant*100)*16+50)//100
    stm_div=(pclk+baud//2)//baud if baud==921600 else hal_div
    # IDF6.1 ESP32-S3 uart_ll: sclk_div=ceil(40MHz/(4095*baud)),
    # fractional divider is floor((sclk<<4)/(baud*sclk_div)). XTAL is40MHz.
    xtal=40000000;sclk_div=(xtal+4095*baud-1)//(4095*baud)
    esp_div16=(xtal<<4)//(baud*sclk_div)
    stm=pclk/stm_div;esp=xtal*16/(esp_div16*sclk_div)
    combined=application_bytes_per_second+data2_bytes_per_second
    return dict(requested_baud=baud,format='8N1',stm32_pclk1_hz=pclk,stm32_bundled_hal_brr=hal_div,stm32_brr=stm_div,stm32_nominal_actual_baud=stm,
                stm32_error_percent=(stm/baud-1)*100,esp32_source='XTAL40MHz',esp32_sclk_div=sclk_div,
                esp32_divider_sixteenths=esp_div16,esp32_nominal_actual_baud=esp,esp32_error_percent=(esp/baud-1)*100,
                endpoint_difference_percent=(esp/stm-1)*100,theoretical_requested_capacity_bytes_per_second=baud/10,
                stm32_nominal_actual_capacity_bytes_per_second=stm/10,observed_old_application_bytes_per_second=application_bytes_per_second,
                additional_DATA2_upper_bound_bytes_per_second=data2_bytes_per_second,combined_budget_bytes_per_second=combined,
                requested_capacity_utilization_percent=combined/(baud/10)*100,actual_stm32_capacity_utilization_percent=combined/(stm/10)*100,
                limitation='Nominal divider math excludes oscillator tolerance, TX interrupt pauses and physical error measurements')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baud',type=int,choices=[115200,921600],default=921600)
    p.add_argument('--observed-bps',type=float,default=11440.433373078635);a=p.parse_args()
    print(json.dumps(budget(a.baud,a.observed_bps),indent=2))

if __name__=='__main__':main()
