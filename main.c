/*****************************************************************************
 * File Name: main.c
 *
 * Description: This code example features a 5-segment CapSense slider.
 *              the slider controls the brightness of the LED and the
 *              current brightness of the LED is published to AWS console.
 *
 * Related Document: See README.md
 *
 *
 ******************************************************************************
 * $ Copyright 2023 Cypress Semiconductor $
 *******************************************************************************/

/*******************************************************************************
 * Include header files
 ******************************************************************************/
#include "CCM.h"
#include "cycfg.h"
#include "cycfg_capsense.h"
#include "led.h"

/*******************************************************************************
 * Macros
 *******************************************************************************/
/* EZI2C interrupt priority must be \
 * higher than CapSense interrupt */
#define CAPSENSE_INTR_PRIORITY (7u)

#define EZI2C_INTR_PRIORITY (6u)

#define TX_BUF_SIZE (200)

/* define MODIFY_SSID macro as 1 for disconnecting the existing Wi-Fi connection and change to new Access point */
#define MODIFY_SSID_AFTER_CONNECTED (0)

/* define CIRRENT_APP_ONBOARDING macro as 1 for Wi-Fi Onboarding via Cirrent APP.*/
#define CIRRENT_APP_ONBOARDING (0)

/*define AWS_FLOW macro as 1 for choosing AWS flow and 0 for Cirrent flow*/
#define AWS_FLOW (1)

/*delay in milliseconds for sending capsense data*/
#define CAPSENSE_DATA_SEND_DELAY (100)

/*Max response delay in milliseconds for AT commands*/
#define RESPONSE_DELAY (120000)

#define POLLING_DELAY (60000)

#define SUCCESS 1

#define FAILURE 0

/* Set SSID, Passphrase and Endpoint as follows
 * AT+CONF SSID=XXXX\n; where XXXX is the required SSID
 * AT+CONF Passphrase=YYYY\n ; YYYY is the Passphrase
 * AT+CONF EndPoint=ZZZZ\n; ZZZZ is the endpoint
 */

#define SET_SSID "AT+CONF SSID=\n"
#define SET_PASSPHRASE "AT+CONF Passphrase=\n"
#define SET_ENDPOINT "AT+CONF Endpoint=\n"

/*******************************************************************************
 * Function Prototypes
 *******************************************************************************/
static uint32_t initialize_capsense(void);
static void process_touch(void);
static void initialize_capsense_tuner(void);
static void capsense_isr(void);
static void capsense_callback();
static void init_AWS();
static void aws_publish();
static void wifionboarding();

/*******************************************************************************
 * Global Variables
 *******************************************************************************/
cy_stc_scb_ezi2c_context_t ezi2c_context;
cyhal_ezi2c_t sEzI2C;
cyhal_ezi2c_slave_cfg_t sEzI2C_sub_cfg;
cyhal_ezi2c_cfg_t sEzI2C_cfg;
volatile bool capsense_scan_complete = false;
int brightness_perc;
int result_ccm = 0;

/*******************************************************************************
 * Function Name: main
 *******************************************************************************
 * Summary:
 *  System entrance point. This function performs
 *  - initial setup of device
 *  - initialize CapSense
 *  - initialize tuner communication
 *  - Connect to AWS IoT core
 *  - scan touch input continuously and update the LED accordingly.
 *
 * Return:
 *  int
 *
 *******************************************************************************/
int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    init_AWS();
    initialize_led();
    initialize_capsense_tuner();
    result = initialize_capsense();

    if (CYRET_SUCCESS != result)
    {
        /* Halt the CPU if CapSense initialization failed */
        CY_ASSERT(0);
    }

    /* Initiate first scan */
    Cy_CapSense_ScanAllWidgets(&cy_capsense_context);

    for (;;)
    {
        if (capsense_scan_complete)
        {
            /* Process all widgets */
            Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);

            /* Process touch input */
            process_touch();

            /* Establishes synchronized operation between the CapSense
             * middleware and the CapSense Tuner tool.
             */
            Cy_CapSense_RunTuner(&cy_capsense_context);

            /* Initiate next scan */
            Cy_CapSense_ScanAllWidgets(&cy_capsense_context);

            capsense_scan_complete = false;
        }
    }
}

/*******************************************************************************
 * Function Name: process_touch
 ********************************************************************************
 * Summary:
 *  Gets the details of touch position detected, processes the touch input
 *  and updates the LED status.
 *
 *******************************************************************************/
static void process_touch(void)
{

    cy_stc_capsense_touch_t *slider_touch_info;
    uint16_t slider_pos;
    uint8_t slider_touch_status;
    bool led_update_req = false;
    static uint16_t slider_pos_prev;
    static led_data_t led_data = {LED_ON, LED_MAX_BRIGHTNESS};
    uint32_t brightness_prev_perc = 0;

    /* Get slider status */
    slider_touch_info = Cy_CapSense_GetTouchInfo(
        CY_CAPSENSE_LINEARSLIDER0_WDGT_ID, &cy_capsense_context);
    slider_touch_status = slider_touch_info->numPosition;
    slider_pos = slider_touch_info->ptrPosition->x;

    /* Detect the new touch on slider */
    if ((0 != slider_touch_status) &&
        (slider_pos != slider_pos_prev))
    {
        brightness_perc = led_data.brightness = (slider_pos * 100) / cy_capsense_context.ptrWdConfig[CY_CAPSENSE_LINEARSLIDER0_WDGT_ID].xResolution;
        led_update_req = true;
    }

    /* Update the LED state and publish to AWS-IOT-CORE*/
    if (led_update_req && brightness_prev_perc != brightness_perc)
    {
        update_led_state(&led_data);
        aws_publish();
    }

    /* Update previous touch status */
    slider_pos_prev = slider_pos;
    brightness_prev_perc = brightness_perc;
}

/*******************************************************************************
 * Function Name: initialize_capsense
 ********************************************************************************
 * Summary:
 *  This function initializes the CapSense and configure the CapSense
 *  interrupt.
 *
 *******************************************************************************/
static uint32_t initialize_capsense(void)
{
    uint32_t status = CYRET_SUCCESS;

    /* CapSense interrupt configuration parameters */
    static const cy_stc_sysint_t capSense_intr_config =
        {
            .intrSrc = csd_interrupt_IRQn,
            .intrPriority = CAPSENSE_INTR_PRIORITY,
        };

    /* Capture the CSD HW block and initialize it to the default state. */
    status = Cy_CapSense_Init(&cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    /* Initialize CapSense interrupt */
    cyhal_system_set_isr(csd_interrupt_IRQn, csd_interrupt_IRQn, CAPSENSE_INTR_PRIORITY, &capsense_isr);
    NVIC_ClearPendingIRQ(capSense_intr_config.intrSrc);
    NVIC_EnableIRQ(capSense_intr_config.intrSrc);

    /* Initialize the CapSense firmware modules. */
    status = Cy_CapSense_Enable(&cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    /* Assign a callback function to indicate end of CapSense scan. */
    status = Cy_CapSense_RegisterCallback(CY_CAPSENSE_END_OF_SCAN_E,
                                          capsense_callback, &cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    return status;
}

/*******************************************************************************
 * Function Name: capsense_isr
 ********************************************************************************
 * Summary:
 *  Wrapper function for handling interrupts from CapSense block.
 *
 *******************************************************************************/
static void capsense_isr(void)
{
    Cy_CapSense_InterruptHandler(CYBSP_CSD_HW, &cy_capsense_context);
}

/*******************************************************************************
 * Function Name: capsense_callback()
 ********************************************************************************
 * Summary:
 *  This function sets a flag to indicate end of a CapSense scan.
 *
 * Parameters:
 *  cy_stc_active_scan_sns_t* : pointer to active sensor details.
 *
 *******************************************************************************/
void capsense_callback(cy_stc_active_scan_sns_t *ptrActiveScan)
{
    capsense_scan_complete = true;
}

/*******************************************************************************
 * Function Name: initialize_capsense_tuner
 ********************************************************************************
 * Summary:
 *  Initializes interface between Tuner GUI and PSoC 6 MCU.
 *
 *******************************************************************************/
static void initialize_capsense_tuner(void)
{
    cy_rslt_t result;

    /* Configure Capsense Tuner as EzI2C Slave */
    sEzI2C_sub_cfg.buf = (uint8 *)&cy_capsense_tuner;
    sEzI2C_sub_cfg.buf_rw_boundary = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.buf_size = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.slave_address = 8U;

    sEzI2C_cfg.data_rate = CYHAL_EZI2C_DATA_RATE_400KHZ;
    sEzI2C_cfg.enable_wake_from_sleep = false;
    sEzI2C_cfg.slave1_cfg = sEzI2C_sub_cfg;
    sEzI2C_cfg.sub_address_size = CYHAL_EZI2C_SUB_ADDR16_BITS;
    sEzI2C_cfg.two_addresses = false;

    result = cyhal_ezi2c_init(&sEzI2C, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL, &sEzI2C_cfg);
    if (result != CY_RSLT_SUCCESS)
    {
        handle_error();
    }
}

static void init_AWS()
{

    uart_init();

    printf("\r ******************AIROC™ CCM MQTT-PUBLISH-CAPSENSE-SLIDER******************\n");

#if MODIFY_SSID_AFTER_CONNECTED

    /* AT command for disconnecting from Wi-Fi network */
    at_command_send_receive("AT+CONF? ThingName\n", RESPONSE_DELAY, &result_ccm, NULL);

#endif

#if AWS_FLOW

    /*Check if CCM module already connected to AWS*/
    if (!is_aws_connected())
    {

        /*AT command for sending Device Endpoint*/
        at_command_send_receive(SET_ENDPOINT, RESPONSE_DELAY, &result_ccm, NULL);

        /*Connect to Wi-Fi network if it is not connected already*/
        if (!is_wifi_connected())
        {
            wifionboarding();
        }

        /*AT command for Connecting to AWS Cloud*/
        at_command_send_receive("AT+CONNECT\n", RESPONSE_DELAY, &result_ccm, "OK 1 CONNECTED\r\n");

        if (result_ccm != 1)
        {
            handle_error();
        }
    }

    /* AT command for storing the topic name in Topic1 */
    at_command_send_receive("AT+CONF Topic1=MySubTopic\n", RESPONSE_DELAY, &result_ccm, NULL);

#else

    /*Check if CCM module already connected to AWS*/
    if (!is_aws_connected())
    {

        /*Connect to Wi-Fi network if it is not connected already*/
        if (!is_wifi_connected())
        {
            wifionboarding();
        }

        /*AT command for Connecting CCM device to AWS staging*/
        at_command_send_receive("AT+CONNECT\n", RESPONSE_DELAY, &result_ccm, NULL);

        /*AT command for Getting Endpoint from Cirrent Cloud*/
        at_command_send_receive("AT+CLOUD_SYNC\n", RESPONSE_DELAY, &result_ccm, NULL);

        /* Check in Cirrent console if the Job executed succesfully*/
        printf("\nThe Connection Automatically switches to the new endpoint after 120 seconds\n\n");

        delay_ms(RESPONSE_DELAY);

        while (!is_aws_connected())
            ;
    }

    /* AT command for storing the topic name in Topic1 */
    at_command_send_receive("AT+CONF Topic1=/MySubTopic\n", RESPONSE_DELAY, &result_ccm, NULL);

#endif
}

/*******************************************************************************
 * Function Name: wifionboarding
 ********************************************************************************
 * Summary: Send AT commands to set SSID and Passphrase for CCM module.
 *                                  or
 *          Send AT command to enter Onboarding mode and connect to Wi-Fi via Cirrent APP
 * Return:
 *  void
 *
 *******************************************************************************/

static void wifionboarding()
{

#if CIRRENT_APP_ONBOARDING

    /* AT command to enter Wi-Fi onboarding mode*/
    at_command_send_receive("AT+CONFMODE\n", RESPONSE_DELAY, &result_ccm, NULL);

    printf("\n\rOpen Cirrent APP on your mobile device and choose your Wi-Fi SSID. \n\rThe program continues after successfully connecting to Wi-Fi SSID.\n\r");

    while (!is_wifi_connected())
    {

        delay_ms(POLLING_DELAY);
    }

#else

    /* AT command for sending SSID */
    at_command_send_receive(SET_SSID, RESPONSE_DELAY, &result_ccm, NULL);

    /*AT command for sending Passphrase*/
    at_command_send_receive(SET_PASSPHRASE, RESPONSE_DELAY, &result_ccm, NULL);

#endif
}

/*******************************************************************************
 * Function Name: aws_publish
 ********************************************************************************
 * Summary: Send AT command for publishing data to AWS IoT Core.
 *
 * Return:
 *  void
 *
 *******************************************************************************/

static void aws_publish()
{

    char buffer[TX_BUF_SIZE];

    memset(buffer, '\0', sizeof(buffer));

    sprintf((char *)buffer, "AT+SEND1 %d\n", brightness_perc);

    /* Send AT command for sending message to AWS cloud */
    at_command_send_receive((char *)buffer, CAPSENSE_DATA_SEND_DELAY, &result_ccm, NULL);
}

/* [] END OF FILE */
