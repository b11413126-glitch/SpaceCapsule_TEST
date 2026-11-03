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
#include <stdlib.h>
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
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/*
 * SpaceCapsule
 * PA7 : BACKUP LED
 * PB0 : ALARM LED
 * PA6 : TIM3 CH1 PWM
 * PC13: B1, rising-edge EXTI
 * USART2: 115200, 8N1
 * TIM2: 1 ms update interrupt
 */


extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;

#define RX_SIZE         256U
#define LINE_SIZE       64U
#define MISSION_MS      180000U
#define STABLE_MS       5000U
#define DEBOUNCE_MS     50U

typedef enum
{
    STATE_IDLE = 0,
    STATE_RUNNING,
    STATE_SUCCESS,
    STATE_FAILED
} MissionState;

/* Interrupt-shared variables */
static volatile uint32_t app_ms = 0;
static volatile uint8_t button_pending = 0;
static volatile uint32_t button_time = 0;

static uint8_t rx_byte;
static volatile uint8_t rx_buffer[RX_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;
static volatile uint8_t rx_overflow = 0;
static volatile uint8_t uart_error = 0;

/* Main-loop-owned variables */
static MissionState mission_state = STATE_IDLE;
static uint8_t backup_on = 0;
static uint8_t power_percent = 0;
static uint8_t stable_active = 0;

static uint32_t mission_start = 0;
static uint32_t stable_start = 0;
static uint32_t finish_elapsed = 0;
static uint32_t last_report = 0;

static char command_line[LINE_SIZE];
static uint16_t command_length = 0;
static uint8_t discard_line = 0;

static void App_Send(const char *text)
{
    /* Only called from main loop, never from an ISR. */
    HAL_UART_Transmit(&huart2,
                     (uint8_t *)text,
                     (uint16_t)strlen(text),
                     100);
}

static const char *App_StateName(void)
{
    switch (mission_state)
    {
        case STATE_IDLE:    return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        case STATE_SUCCESS: return "SUCCESS";
        case STATE_FAILED:  return "FAILED";
        default:            return "UNKNOWN";
    }
}

static uint32_t App_Elapsed(uint32_t now)
{
    if (mission_state == STATE_IDLE)
        return 0;

    if (mission_state == STATE_RUNNING)
        return now - mission_start;

    return finish_elapsed;
}

static uint32_t App_StableTime(uint32_t now)
{
    uint32_t elapsed;

    if (!stable_active)
        return 0;

    if (mission_state == STATE_SUCCESS)
        return STABLE_MS;

    if (mission_state != STATE_RUNNING)
        return 0;

    elapsed = now - stable_start;

    return (elapsed >= STABLE_MS) ? STABLE_MS : elapsed;
}

static void App_Report(uint32_t now)
{
    char text[180];
    uint32_t elapsed = App_Elapsed(now);
    uint32_t remaining;
    uint32_t stable = App_StableTime(now);

    if (elapsed > MISSION_MS)
        elapsed = MISSION_MS;

    remaining = MISSION_MS - elapsed;

    snprintf(text, sizeof(text),
             "STATUS state=%s remaining_ms=%lu "
             "backup=%u power=%u stable_ms=%lu ready=%u\r\n",
             App_StateName(),
             (unsigned long)remaining,
             (unsigned int)backup_on,
             (unsigned int)power_percent,
             (unsigned long)stable,
             (unsigned int)(
                 mission_state == STATE_RUNNING &&
                 backup_on &&
                 power_percent == 60 &&
                 stable >= STABLE_MS));

    App_Send(text);
}

static void App_SetPower(uint8_t value)
{
    uint32_t period = __HAL_TIM_GET_AUTORELOAD(&htim3) + 1U;
    uint32_t compare = (period * value) / 100U;

    power_percent = value;

    /* ARR=999: 60% -> CCR1=600; 100% -> CCR1=1000. */
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, compare);
}

static void App_UpdateLEDs(uint32_t now)
{
    GPIO_PinState alarm = GPIO_PIN_RESET;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7,
                      backup_on ? GPIO_PIN_SET : GPIO_PIN_RESET);

    if (mission_state == STATE_FAILED)
    {
        alarm = GPIO_PIN_SET;
    }
    else if (mission_state == STATE_RUNNING)
    {
        /* 250 ms on + 250 ms off = 2 Hz. */
        alarm = (((now - mission_start) / 250U) % 2U == 0U)
                ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, alarm);
}

static void App_CheckTimeout(uint32_t now)
{
    if (mission_state == STATE_RUNNING &&
        now - mission_start >= MISSION_MS)
    {
        mission_state = STATE_FAILED;
        finish_elapsed = MISSION_MS;
        stable_active = 0;

        App_UpdateLEDs(now);
        App_Send("EVENT TIMEOUT FAILED\r\n");
        App_Report(now);
    }
}

static void App_Reset(void)
{
    mission_state = STATE_IDLE;
    backup_on = 0;
    stable_active = 0;
    mission_start = 0;
    stable_start = 0;
    finish_elapsed = 0;

    App_SetPower(0);
    App_UpdateLEDs(app_ms);
}

/* Strictly accept POWER followed by an integer from 0 to 100. */
static int App_ParsePower(const char *line, uint8_t *value)
{
    const char *number;
    char *end;
    long parsed;

    if (strncmp(line, "POWER ", 6) != 0)
        return 0;

    number = line + 6;

    /* Reject signs, missing values and excessively long numbers. */
    if (*number < '0' || *number > '9' || strlen(number) > 3U)
        return 0;

    parsed = strtol(number, &end, 10);

    if (*end != '\0' || parsed < 0 || parsed > 100)
        return 0;

    *value = (uint8_t)parsed;
    return 1;
}

static void App_Command(const char *line)
{
    uint32_t now = app_ms;
    uint8_t value;

    App_CheckTimeout(now);

    if (strcmp(line, "STATUS") == 0)
    {
        App_Report(now);
        return;
    }

    if (strcmp(line, "START") == 0)
    {
        if (mission_state != STATE_IDLE)
        {
            App_Send("ERR START_REQUIRES_IDLE\r\n");
            return;
        }

        backup_on = 0;
        stable_active = 0;
        finish_elapsed = 0;
        App_SetPower(0);

        mission_start = app_ms;
        mission_state = STATE_RUNNING;
        App_UpdateLEDs(mission_start);

        App_Send("OK START\r\n");
        App_Report(app_ms);
        return;
    }

    if (strcmp(line, "RESET") == 0)
    {
        if (mission_state != STATE_SUCCESS &&
            mission_state != STATE_FAILED)
        {
            App_Send("ERR RESET_REQUIRES_FINISHED\r\n");
            return;
        }

        App_Reset();
        App_Send("OK RESET\r\n");
        App_Report(app_ms);
        return;
    }

    if (strcmp(line, "BACKUP ON") == 0)
    {
        if (mission_state != STATE_RUNNING)
        {
            App_Send("ERR NOT_RUNNING\r\n");
            return;
        }

        backup_on = 1;
        App_UpdateLEDs(app_ms);
        App_Send("OK BACKUP_ON\r\n");
        App_Report(app_ms);
        return;
    }

    if (App_ParsePower(line, &value))
    {
        if (mission_state != STATE_RUNNING)
        {
            App_Send("ERR NOT_RUNNING\r\n");
            return;
        }

        if (!backup_on)
        {
            App_Send("ERR BACKUP_REQUIRED\r\n");
            return;
        }

        App_SetPower(value);

        if (value == 60)
        {
            /* Repeated POWER 60 must not restart stable timing. */
            if (!stable_active)
            {
                stable_start = app_ms;
                stable_active = 1;
            }
        }
        else
        {
            stable_active = 0;
        }

        App_Send("OK POWER\r\n");
        App_Report(app_ms);
        return;
    }

    App_Send("ERR COMMAND_USE_START_BACKUP_ON_POWER_N_STATUS_RESET\r\n");
}

static void App_ProcessButton(void)
{
    uint8_t pending;
    uint32_t pressed_at;
    uint32_t mask = __get_PRIMASK();

    /* Atomically take the event and its timestamp. */
    __disable_irq();
    pending = button_pending;
    pressed_at = button_time;
    button_pending = 0;
    __set_PRIMASK(mask);

    if (!pending)
        return;

    if (mission_state != STATE_RUNNING)
    {
        App_Send("ERR BUTTON_NOT_RUNNING\r\n");
        return;
    }

    if (pressed_at - mission_start >= MISSION_MS)
    {
        App_CheckTimeout(app_ms);
        return;
    }

    if (!backup_on)
    {
        App_Send("ERR BUTTON_BACKUP_REQUIRED\r\n");
        return;
    }

    if (power_percent != 60 || !stable_active)
    {
        App_Send("ERR BUTTON_POWER_MUST_BE_60\r\n");
        return;
    }

    /*
     * A very old button event must not qualify against a later
     * stable_start timestamp. Unsigned subtraction handles wraparound.
     */
    uint32_t stable = pressed_at - stable_start;

    if (stable >= 0x80000000UL || stable < STABLE_MS)
    {
        App_Send("ERR BUTTON_WAIT_5_SECONDS\r\n");
        return;
    }

    mission_state = STATE_SUCCESS;
    finish_elapsed = pressed_at - mission_start;

    App_UpdateLEDs(app_ms);
    App_Send("EVENT RESTART_SUCCESS\r\n");
    App_Report(app_ms);
}

static int App_ReadByte(uint8_t *value)
{
    if (rx_tail == rx_head)
        return 0;

    *value = rx_buffer[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1U) % RX_SIZE);
    return 1;
}

static void App_ProcessUART(void)
{
    uint8_t value;

    if (rx_overflow || uart_error)
    {
        uint32_t mask = __get_PRIMASK();

        __disable_irq();
        rx_tail = rx_head;
        rx_overflow = 0;
        uart_error = 0;
        __set_PRIMASK(mask);

        command_length = 0;
        discard_line = 1;

        App_Send("ERR RX_LOST_SEND_NEWLINE_THEN_RETRY\r\n");
    }

    while (App_ReadByte(&value))
    {
        /* Process a physical button event before another command. */
        App_ProcessButton();

        if (value == '\r' || value == '\n')
        {
            if (discard_line)
            {
                discard_line = 0;
                command_length = 0;
            }
            else if (command_length > 0)
            {
                command_line[command_length] = '\0';
                App_Command(command_line);
                command_length = 0;
            }

            continue;
        }

        if (discard_line)
            continue;

        if (value < 32U || value > 126U)
        {
            command_length = 0;
            discard_line = 1;
            App_Send("ERR ASCII_COMMANDS_ONLY\r\n");
            continue;
        }

        if (command_length < LINE_SIZE - 1U)
        {
            command_line[command_length++] = (char)value;
        }
        else
        {
            command_length = 0;
            discard_line = 1;
            App_Send("ERR COMMAND_TOO_LONG\r\n");
        }
    }
}

static void App_Init(void)
{
    App_Reset();

    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK)
        Error_Handler();

    if (HAL_UART_Receive_IT(&huart2, &rx_byte, 1) != HAL_OK)
        Error_Handler();

    if (HAL_TIM_Base_Start_IT(&htim2) != HAL_OK)
        Error_Handler();

    last_report = app_ms;

    App_Send("\r\nSPACE_CAPSULE READY\r\n");
    App_Send("COMMANDS: START | BACKUP ON | POWER N | STATUS | RESET\r\n");
    App_Report(app_ms);
}

static void App_Run(void)
{
    /*
     * Check a recorded button press before checking the current timeout.
     * A qualifying press before the deadline may therefore succeed.
     */
    App_ProcessButton();
    App_CheckTimeout(app_ms);
    App_ProcessUART();
    App_CheckTimeout(app_ms);
    App_UpdateLEDs(app_ms);

    uint32_t now = app_ms;

    if (mission_state == STATE_RUNNING &&
        now - last_report >= 250U)
    {
        last_report = now;
        App_Report(now);
    }
}

/* TIM2 ISR callback: keep it short. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2)
        app_ms++;
}

/* UART ISR callback: receive into a ring buffer only. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        uint16_t next = (uint16_t)((rx_head + 1U) % RX_SIZE);

        if (next != rx_tail)
        {
            rx_buffer[rx_head] = rx_byte;
            rx_head = next;
        }
        else
        {
            rx_overflow = 1;
        }

        HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        uart_error = 1;

        /* Restart if HAL ended reception because of an error. */
        if (huart->RxState == HAL_UART_STATE_READY)
            HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
    }
}

/* B1 ISR callback: debounce and record the event. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    static uint32_t last_press = 0;
    static uint8_t first_press = 1;

    if (GPIO_Pin == GPIO_PIN_13)
    {
        uint32_t now = app_ms;

        if (first_press || now - last_press >= DEBOUNCE_MS)
        {
            first_press = 0;
            last_press = now;

            /* Preserve the earliest unprocessed press. */
            if (!button_pending)
            {
                button_time = now;
                button_pending = 1;
            }
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
  MX_TIM3_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  App_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  App_Run();
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 169;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

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
  HAL_GPIO_WritePin(BACKUP_LED_GPIO_Port, BACKUP_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : BACKUP_LED_Pin */
  GPIO_InitStruct.Pin = BACKUP_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BACKUP_LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

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
