/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include <stdio.h>
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
/* 狀態機定義：定義系統目前的三種運作模式 */
typedef enum {
    STATE_IDLE,       // 1. 待命巡航：綠燈常亮，等待 2.5~4.0 秒隨機抽測
    STATE_TESTING,    // 2. 抽測進行中：綠燈滅、黃燈 2Hz 慢閃，等待駕駛按鍵
    STATE_ALERT       // 3. 警報中：黃燈續閃、紅燈 10Hz 超快閃（蜂鳴器替身）
} DriverState_t;

volatile DriverState_t state = STATE_IDLE; // 當前狀態，加 volatile 防止編譯器優化
volatile uint32_t ms_ticks = 0;            // 核心時基：TIM2 每 1ms 遞增 1
volatile uint32_t test_start_ms = 0;       // 記錄每次抽測開始的當下時間戳（毫秒）
volatile uint32_t next_test_delay = 3000;  // 記錄下一次抽測預計發起的時間點

/* 隨機間隔產生函式：模擬真實巡航 25~40 分鐘（Demo 縮放為 2.5~4.0 秒） */
uint32_t get_random_interval(void) {
    // 利用硬體計數器累加的毫秒數來取餘數，產生 2500 ~ 4000 之間的數值
    return 2500 + (ms_ticks % 1501);
}
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* TIM2 1ms 週期溢位中斷（每 1ms 硬體自動跳進來執行一次） */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM2) {
        ms_ticks++; // 核心時鐘前進 1ms

        // 行為 A：若處於抽測狀態，黃燈慢閃 (2 Hz)
        // 2 Hz 代表一秒閃兩次（週期 500ms），即每 250ms 反轉一次電位
        if (state == STATE_TESTING) {
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); // 綠燈熄滅
            if (ms_ticks % 250 == 0) {
                HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); // 反轉 PA6 (黃燈)
            }
        }
        // 行為 B：若處於昏睡警報狀態，黃燈續閃，紅燈超急促暴閃 (10 Hz)
        // 10 Hz 代表每 50ms 反轉一次電位，模擬刺耳的警報蜂鳴器
        else if (state == STATE_ALERT) {
            if (ms_ticks % 250 == 0) {
                HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); // 黃燈維持閃爍
            }
            if (ms_ticks % 50 == 0) {
                HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_7); // 反轉 PA7 (紅燈急鳴)
            }
        }
    }
}

/* EXTI 外部中斷（按下 PC13 藍色按鈕 B1 的瞬間觸發） */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == GPIO_PIN_13) {
        static uint32_t last_btn_ms = 0;

        // 1. 軟體去彈跳：若兩次中斷觸發間隔小於 50ms，視為機械金屬彈跳雜訊直接丟棄
        if (ms_ticks - last_btn_ms < 50) return;
        last_btn_ms = ms_ticks; // 記錄本次有效按下的時間

        // 2. 情境一：抽測期間按下按鍵（判定駕駛清醒）
        if (state == STATE_TESTING) {
            uint32_t reaction_time = ms_ticks - test_start_ms; // 計算花費了多少毫秒

            // 透過 UART 印出雲端通過日誌
            char msg[64];
            snprintf(msg, sizeof(msg), "[PASS] Driver Clear! Reaction: %lu ms\r\n", reaction_time);
            HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);

            // 燈號復歸：綠燈亮、黃燈紅燈熄滅
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);

            // 排定下一次抽測時間，並將系統切回 IDLE 待命模式
            next_test_delay = ms_ticks + get_random_interval();
            state = STATE_IDLE;
        }
        // 3. 情境二：在警報暴閃狀態下按鍵（駕駛被喚醒，手動解除警報）
        else if (state == STATE_ALERT) {
            char *msg = "[RESET] Alert cleared by driver. Resuming cruise...\r\n";
            HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);

            // 燈號復歸待命：綠燈常亮、黃紅燈熄滅
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);

            // 重新排定下次抽測，回到 IDLE 待命
            next_test_delay = ms_ticks + get_random_interval();
            state = STATE_IDLE;
        }
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
    // 1. 設定開機預設燈號：綠燈亮起（待命巡航），黃紅燈熄滅
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);

    // 2. 啟動 TIM2 週期中斷計時器（如果沒寫這行，TIM2 就不會開始計數）
    HAL_TIM_Base_Start_IT(&htim2);

    // 3. 透過 USART2 向終端機發送開機自檢完成訊息
    char *boot_msg = "\r\n=== Driver Vigilance Alert System Started ===\r\nCruise state: Safe.\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t*)boot_msg, strlen(boot_msg), 100);
    /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    /* Infinite loop */
      /* USER CODE BEGIN WHILE */
      while (1)
      {
        /* USER CODE END WHILE */

    	  /* USER CODE BEGIN 3 */
    	    //  1：檢查待命時間是否到了（2.5 ~ 4.0 秒），到了就啟動抽測
    	    if (state == STATE_IDLE && ms_ticks >= next_test_delay) {
    	        state = STATE_TESTING;     // 切換至抽測狀態（觸發中斷裡的黃燈慢閃）
    	        test_start_ms = ms_ticks;  // 記錄抽測開始的當下時間

    	        char *msg = "\r\n>>> [ATTENTION TEST] Yellow light flashing! Press B1 within 5s! <<<\r\n";
    	        HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
    	    }

    	    // 2：檢查抽測是否超過 10 秒完全無反應（判定微睡眠昏迷）
    	    if (state == STATE_TESTING && (ms_ticks - test_start_ms >= 10000)) {
    	        state = STATE_ALERT;       // 切換至警報狀態（觸發中斷裡的紅燈暴閃）
    	        char *msg = "[ALERT CRITICAL] Unresponsive for >10s! Microsleep detected! Buzzer ON!\r\n";
    	        HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
    	    }
    	    /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 169;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 999;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart2, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart2, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PA5 PA6 PA7 */
  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
