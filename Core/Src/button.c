/*
 * Button.c
 * Button handler
 * Allows to handle single, double, or long presses.
 * Uses the STM32 HAL library.
 * Created on: Aug 15, 2024
 *     Author: k.rudenko
 */

/******************************************************************************
 *                                 INCLUDES                                   *
 ******************************************************************************/

#include "Button.h"
#include <string.h>

/******************************************************************************
 *                           LOCAL DATA TYPES                                 *
 ******************************************************************************/

typedef enum
{
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_SINGLE_PRESS,
    BUTTON_EVENT_DOUBLE_PRESS,
    BUTTON_EVENT_LONG_PRESS
} ButtonEvent;

typedef enum
{
    BUTTON_IDLE,
    BUTTON_PRESSED,
    BUTTON_RELEASED,
    BUTTON_DOUBLE_CHECK
} ButtonState;

typedef struct btn
{
    GPIO_TypeDef *GPIO_Port;               // GPIO Port of the button
    uint16_t GPIO_Pin;                     // GPIO Pin of the button
    ButtonState state;                     // State machine state
    uint32_t press_ts;                     // Timestamp when the button was pressed
    uint32_t release_ts;                   // Timestamp when the button was released
    uint8_t press_cnt;                     // Count for detecting double press
    ButtonEvent event;                     // Current event for this button
    ButtonEvent event_flag;                // Current event for this button
    GPIO_PinState active_level;            // Active level (high or low)
    Button_FncPtr_t single_press_callback; // Callback for single press
    Button_FncPtr_t double_press_callback; // Callback for double press
    Button_FncPtr_t long_press_callback;   // Callback for long press
    uint32_t debounce_time;                // Debounce time
    TIM_HandleTypeDef *htim;               // timer handler
} btn_t;

/******************************************************************************
 *                         LOCAL DATA PROTOTYPES                              *
 ******************************************************************************/

/* Memory for button instances */
static volatile struct
{
    btn_t    mem[BUTTON_MAX_INSTANCES];
    uint32_t cnt;
    btn_t*   active_btn;
} btn_alloc;

/******************************************************************************
 *                              GLOBAL FUNCTIONS                              *
 ******************************************************************************/

/* This function is used to initialize a button instance
 * (except for initializing the GPIO, and the ISR that is done by the CubeMX)
 * */
Button_Handler Button_Init(GPIO_TypeDef *GPIO_Port, uint16_t GPIO_Pin,
        GPIO_PinState active_level, Button_FncPtr_t single_press_cb,
        Button_FncPtr_t double_press_cb, Button_FncPtr_t long_press_cb,
        TIM_HandleTypeDef *htim)
{
    Button_Handler *retVal;

    /* Check if we are able to allocate more buttons */
    if (btn_alloc.cnt < BUTTON_MAX_INSTANCES)
    {
        /* Initialize a new button instance */
        btn_t *btn = (btn_t *)&btn_alloc.mem[btn_alloc.cnt++];
        btn->GPIO_Port = GPIO_Port;
        btn->GPIO_Pin = GPIO_Pin;
        btn->state = BUTTON_IDLE;
        btn->press_ts = 0U;
        btn->release_ts = 0U;
        btn->press_cnt = 0U;
        btn->event = BUTTON_EVENT_NONE;
        btn->active_level = active_level;
        btn->single_press_callback = single_press_cb;
        btn->double_press_callback = double_press_cb;
        btn->long_press_callback = long_press_cb;
        btn->debounce_time = 0U;
        btn->htim = htim;
        /* Return memory address of button instance */
        retVal = (Button_Handler) btn;
    }
    else
    {
        /* No more space for new buttons */
        retVal = NULL;
    }

    return retVal;
}

/* This function is used to handle the interrupt of this button */
/* NOTE: This interrupt handler shall be invoked on falling AND rising edges! */
void Button_HandleInterrupt(Button_Handler handle)
{
    GPIO_PinState pin_state;
    uint32_t now;
    btn_t *btn;

    if (handle != NULL)
    {
        btn = (btn_t*) handle;
        /* make a timestamp */
        now = HAL_GetTick();

        /* check for debounce */
        if ((now - btn->debounce_time) > BUTTON_DEBOUNCE_TIME_MS)
        {
            /* Get pin state */
            pin_state = HAL_GPIO_ReadPin(btn->GPIO_Port, btn->GPIO_Pin);

            if (btn->active_level == pin_state)
            {
                /* Start a timer to invoke the Button_Process() function every 10 ms */
                __HAL_TIM_SET_COUNTER(btn->htim, 0U);
                HAL_TIM_Base_Start_IT(btn->htim);

                btn->press_ts = now;
                btn->state = BUTTON_PRESSED;
            }
            else
            {
                btn->release_ts = now;
                btn->state = BUTTON_RELEASED;
            }

            /* Update debounce time */
            btn->debounce_time = now;
            /* Update active button pointer */
            btn_alloc.active_btn = btn;
        }
    }
}

/* This function is used to analyze buttons inside the Timer ISR each 10ms */
void Button_Process(void)
{
    uint32_t now = HAL_GetTick();
    btn_t *btn;

    /* Get active button handler */
    btn = btn_alloc.active_btn;

    /* Analyze this button */
    switch (btn->state)
    {
        case BUTTON_RELEASED:
            if ((now - btn->press_ts) > BUTTON_LONG_PRESS_TIME_MS)
            {
                btn->event = BUTTON_EVENT_LONG_PRESS;
                btn->state = BUTTON_IDLE;
                /* Reset active button pointer */
                btn_alloc.active_btn = NULL;
                /* Stop the timer because the button analysis is complete -
                 * no need for further invokes of this function */
                HAL_TIM_Base_Stop_IT(btn->htim);
            }
            else if ((btn->release_ts - btn->press_ts) <= BUTTON_SHORT_PRESS_TIME_MS)
            {
                btn->press_cnt++;
                btn->state = BUTTON_DOUBLE_CHECK;
            }
            else
            {
                btn->event = BUTTON_EVENT_SINGLE_PRESS;
                btn->state = BUTTON_IDLE;
                /* Reset active button pointer */
                btn_alloc.active_btn = NULL;
                /* Stop the timer because the button analysis is complete -
                 * no need for further invokes of this function */
                HAL_TIM_Base_Stop_IT(btn->htim);
            }
            break;

        case BUTTON_DOUBLE_CHECK:
            if ((now - btn->release_ts) > BUTTON_DOUBLE_PRESS_TIME_MS)
            {
                if (btn->press_cnt == 1)
                {
                    btn->event = BUTTON_EVENT_SINGLE_PRESS;
                }
                else if (btn->press_cnt == 2)
                {
                    btn->event = BUTTON_EVENT_DOUBLE_PRESS;
                }
                btn->press_cnt = 0;
                btn->state = BUTTON_IDLE;
                /* Reset active button pointer */
                btn_alloc.active_btn = NULL;
                /* Stop the timer because the button analysis is complete -
                 * no need for further invokes of this function */
                HAL_TIM_Base_Stop_IT(btn->htim);
            }
            break;

        default:
            break;
    }

    // Handle the event in the main loop context
    if (btn->event != BUTTON_EVENT_NONE)
    {
        /* Set the event flag for the main loop */
        btn->event_flag = btn->event;
        /* Clear the event */
        btn->event = BUTTON_EVENT_NONE;
    }
}

/* This function is used to asynchronously call registered users callbacks in thread mode */
void Button_Main(void)
{
    btn_t *btn;

    for (uint32_t idx = 0U; idx < btn_alloc.cnt; idx++)
    {
        btn = (btn_t *)&btn_alloc.mem[idx];

        /* Check and clear event from button */
        __disable_irq();
        ButtonEvent event = btn->event_flag;
        btn->event_flag = BUTTON_EVENT_NONE;
        __enable_irq();

        switch (event)
        {
            case BUTTON_EVENT_SINGLE_PRESS:
            {
                if (btn->single_press_callback != NULL)
                {
                    btn->single_press_callback();
                }

                break;
            }

            case BUTTON_EVENT_DOUBLE_PRESS:
            {
                if (btn->double_press_callback != NULL)
                {
                    btn->double_press_callback();
                }

                break;
            }

            case BUTTON_EVENT_LONG_PRESS:
            {
                if (btn->long_press_callback != NULL)
                {
                    btn->long_press_callback();
                }

                break;
            }

            default:
                break;
        }
    }
}