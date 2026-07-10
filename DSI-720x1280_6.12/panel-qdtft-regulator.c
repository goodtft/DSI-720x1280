// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 QDtft
 *
 * Based on rpi-panel-v2-regulator.c by Dave Stevenson <dave.stevenson@raspberrypi.com>
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

/* I2C registers of the microcontroller. */
#define REG_LCD_CTR	0x61
#define REG_BL_PWM	0x62
#define REG_ID		0x63


#define CMD_CHALL 0x70
#define CMD_RESP  0x71 

#define NUM_GPIO	8 

#define DELTA  0x38505342U
static const uint32_t KEY[4] = {0x71129BBD, 0x2DC6FB73, 0x5B2BF004, 0x1D272E25};

struct qd_panel_lcd {
	/* lock to serialise overall accesses to the Atmel */
	struct mutex	lock;
	struct regmap	*regmap;
	u8 current_state;
	struct gpio_chip gc;
	struct gpio_desc *bl_enable;
};

static const struct regmap_config qd_panel_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_BL_PWM,
};

static void qd_Enc(u32 *v)
{
    u32 v0=v[0],v1=v[1],sum=0;
    for(u8 i=0;i<32;i++)
    {
        sum+=DELTA;
        v0 += ((v1<<4)+KEY[0]) ^ (v1+sum) ^ ((v1>>5)+KEY[1]);
        v1 += ((v0<<4)+KEY[2]) ^ (v0+sum) ^ ((v0>>5)+KEY[3]);
    }
    v[0]=v0;v[1]=v1;
}

static int qd_panel_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static void qd_panel_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct qd_panel_lcd *lcd_state = gpiochip_get_data(gc);
	u8 last_val;

	if (off >= NUM_GPIO)
		return;

	mutex_lock(&lcd_state->lock);

	last_val = lcd_state->current_state;
	if (val)
		last_val |= (1 << off);
	else
		last_val &= ~(1 << off);

	lcd_state->current_state = last_val;

	regmap_write(lcd_state->regmap, REG_LCD_CTR, last_val);

	mutex_unlock(&lcd_state->lock);
}

static int qd_panel_update_status(struct backlight_device *bl)
{
	struct qd_panel_lcd *lcd_state = bl_get_data(bl);
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;
	if(lcd_state->bl_enable) {
		if(brightness)
			gpiod_set_value_cansleep(lcd_state->bl_enable, 1);
		else
			gpiod_set_value_cansleep(lcd_state->bl_enable, 0);
	}

	return regmap_write(lcd_state->regmap, REG_BL_PWM, brightness);
}

static const struct backlight_ops qd_panel_bl = {
	.update_status	= qd_panel_update_status,
};

static int qd_panel_i2c_write(struct i2c_client *client, u8 reg, u8 *buf, unsigned int len)
{
	struct i2c_msg msgs;
	u8 *send_buf;
	int ret;

	send_buf = kmalloc(len + 1, GFP_KERNEL);
	if (!send_buf)
		return -ENOMEM;

	send_buf[0] = reg;
	memcpy(&send_buf[1], buf, len);

	/* Write register address */
	msgs.addr = client->addr;
	msgs.flags = 0;
	msgs.len = len + 1;
	msgs.buf = send_buf;

	ret = i2c_transfer(client->adapter, &msgs, 1);

	kfree(send_buf);

	if (ret != 1)
		return -EIO;

	return 0;
}

static int qd_panel_i2c_read(struct i2c_client *client, u8 reg, u8 *buf, unsigned int len)
{
	struct i2c_msg msgs[2];
	u8 addr_buf[1] = { reg };
	int ret;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;
	return 0;
}

/*
 * I2C driver interface functions
 */
static int qd_panel_i2c_probe(struct i2c_client *i2c)
{
	struct backlight_properties props = { };
	struct backlight_device *bl;
	struct qd_panel_lcd *lcd_state;
	struct regmap *regmap;
	u8 data;
	int ret;
	u8 sbuf[8];
	u32 challenge[2];
	u32 response[2];

	lcd_state = devm_kzalloc(&i2c->dev, sizeof(*lcd_state), GFP_KERNEL);
	if (!lcd_state)
		return -ENOMEM;

	mutex_init(&lcd_state->lock);
	i2c_set_clientdata(i2c, lcd_state);

	regmap = devm_regmap_init_i2c(i2c, &qd_panel_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto error;
	}

	get_random_bytes(sbuf, 8);

	ret = qd_panel_i2c_write(i2c, CMD_CHALL, sbuf, 8);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to read CMD_CHALL reg: %d\n", ret);
		goto error;
	}

	challenge[0] = (sbuf[0] << 24)|(sbuf[1]<<16)|(sbuf[2]<<8)|sbuf[3];
	challenge[1] = (sbuf[4] << 24)|(sbuf[5]<<16)|(sbuf[6]<<8)|sbuf[7];

	qd_Enc(challenge);

	msleep(10);

	ret = qd_panel_i2c_read(i2c, CMD_RESP, sbuf, 8);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to read CMD_RESP reg: %d\n", ret);
		goto error;
	}

	response[0] = (sbuf[0] << 24)|(sbuf[1]<<16)|(sbuf[2]<<8)|sbuf[3];
	response[1] = (sbuf[4] << 24)|(sbuf[5]<<16)|(sbuf[6]<<8)|sbuf[7];

	if((challenge[0] != response[0]) || (challenge[1] != response[1]))
	{
		ret = -1;
		dev_err(&i2c->dev, "Failed to Encry: %d\n", ret);
		goto error;
	}

	ret = qd_panel_i2c_read(i2c, REG_ID, &data, 1);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to read REG_ID reg: %d\n", ret);
		goto error;
	}

	switch (data & 0xff) {
	case 0x78:
		break;
	default:
		dev_err(&i2c->dev, "Unknown revision: 0x%02x\n",
			data & 0x0f);
		ret = -ENODEV;
		goto error;
	}

	lcd_state->current_state = 0;//BIT(2) | BIT(3);

	regmap_write(regmap, REG_LCD_CTR, lcd_state->current_state);

	msleep(10);

	lcd_state->regmap = regmap;
	lcd_state->gc.parent = &i2c->dev;
	lcd_state->gc.label = i2c->name;
	lcd_state->gc.owner = THIS_MODULE;
	lcd_state->gc.base = -1;
	lcd_state->gc.ngpio = NUM_GPIO;

	lcd_state->gc.set = qd_panel_gpio_set;
	lcd_state->gc.get_direction = qd_panel_gpio_get_direction;
	lcd_state->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(&i2c->dev, &lcd_state->gc, lcd_state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to create gpiochip: %d\n", ret);
		goto error;
	}

	lcd_state->bl_enable =
			devm_gpiod_get_optional(&i2c->dev, "enable", GPIOD_OUT_LOW);
		if (IS_ERR(lcd_state->bl_enable))
			return dev_err_probe(&i2c->dev, PTR_ERR(lcd_state->bl_enable),
						 "Couldn't get bl enable GPIO number\n");

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	bl = devm_backlight_device_register(&i2c->dev, dev_name(&i2c->dev),
					    &i2c->dev, lcd_state, &qd_panel_bl,
					    &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	bl->props.brightness = 255;

	return 0;

error:
	mutex_destroy(&lcd_state->lock);
	return ret;
}

static void qd_panel_i2c_remove(struct i2c_client *client)
{
	struct qd_panel_lcd *lcd_state = i2c_get_clientdata(client);

	mutex_destroy(&lcd_state->lock);
}

static void qd_panel_i2c_shutdown(struct i2c_client *client)
{
	struct qd_panel_lcd *lcd_state = i2c_get_clientdata(client);
	if(lcd_state->bl_enable)
		gpiod_set_value_cansleep(lcd_state->bl_enable, 0);
	regmap_write(lcd_state->regmap, REG_BL_PWM, 0);
}

static const struct of_device_id qd_panel_dt_ids[] = {
	{ .compatible = "qdtft,touchscreen-panel-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, qd_panel_dt_ids);

static struct i2c_driver qd_panel_regulator_driver = {
	.driver = {
		.name = "qdtft_regulator",
		.of_match_table = of_match_ptr(qd_panel_dt_ids),
	},
	.probe = qd_panel_i2c_probe,
	.remove	= qd_panel_i2c_remove,
	.shutdown = qd_panel_i2c_shutdown,
};

module_i2c_driver(qd_panel_regulator_driver);

MODULE_AUTHOR("QDTFT Team <support@qdtft.com>");
MODULE_DESCRIPTION("Regulator device driver for qdtft touchscreen panel");
MODULE_LICENSE("GPL");

