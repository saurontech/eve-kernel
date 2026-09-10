/*
 * Advantech MSP430 Watchdog driver
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/reboot.h>

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>

#define ADV_WDT_MAX_TIME 6527 /* in seconds */
#define ADV_WDT_DEFAULT_TIME 60 /* in seconds */

#define WDOG_SEC_TO_COUNT(s) ((s) * 10) /* Time unit for register: 100ms */
#define WDOG_COUNT_TO_SEC(s) ((s) / 10) /* Time unit for register: 100ms */

#define ADV_WDT_STATUS_OPEN 0
#define ADV_WDT_STATUS_STARTED 1
#define ADV_WDT_EXPECT_CLOSE 2

/* ECU-150v2 port: renamed from "adv-wdt-i2c" to avoid clashing with the
 * RSB-3720 watchdog (drivers/watchdog/watchdog_advantech.c), which registers
 * an i2c_driver under that same name in this tree. The DT compatible string
 * below is unchanged, so device matching is unaffected. */
#define DRIVER_NAME "adv-wdt-msp430"

#define REG_WDT_WATCHDOG_TIME_OUT 0x15
#define REG_WDT_POWER_OFF_TIME 0x16
#define REG_WDT_INT_PRE_TIME 0x17
#define REG_WDT_REMAIN_TIME_OUT 0x25
#define REG_WDT_REMAIN_PRE_TIME 0x26
#define REG_WDT_VERSION 0x27
#define REG_WDT_POWER_BTN_MODE 0x28

static struct i2c_client *adv_client;

static struct {
	unsigned int timeout;
	unsigned int remain_time;
	unsigned long status;
	int gpio_wdt_ping;
	int wdt_ping_status;
	int gpio_wdt_en;
	int wdt_en_off;
	unsigned char version[2];
	struct gpio_desc *gpiod_wdt_en;
	struct gpio_desc *gpiod_wdt_ping;
} adv_wdt;

static struct miscdevice adv_wdt_miscdev;

static bool nowayout = WATCHDOG_NOWAYOUT;

module_param(nowayout, bool, 0);

MODULE_PARM_DESC(
	nowayout,
	"Watchdog cannot be stopped once started (default=" __MODULE_STRING(
		WATCHDOG_NOWAYOUT) ")");

static unsigned int timeout = ADV_WDT_DEFAULT_TIME;

module_param(timeout, uint, 0);

MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds (default=" __MODULE_STRING(
			 ADV_WDT_DEFAULT_TIME) ")");

static struct watchdog_info adv_wdt_info = {
	.identity = "Advantech watchdog",
	.options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE,
	.firmware_version = 0,
};

static int adv_wdt_i2c_write_reg(struct i2c_client *client, u8 reg, void *buf,
				 size_t len)
{
	u8 *val = kmalloc((1 + len) * sizeof(u8), GFP_KERNEL);
	struct i2c_msg msg[1] = { {
		.addr = client->addr,
		.flags = 0,
		.len = sizeof(val),
		.buf = val,
	} };

	val[0] = reg;
	memcpy(&val[1], buf, len);

	if (i2c_transfer(client->adapter, msg, 1) != 1) {
		dev_err(&client->dev,
			"adv_wdt_i2c_write: i2c transfer failed\n");
		return -EIO;
	}

	msleep(10);
	return 0;
}

static int adv_wdt_i2c_read_reg(struct i2c_client *client, u8 reg, void *buf,
				size_t len)
{
	struct i2c_msg msg[2] = { {
					  .addr = client->addr,
					  .flags = 0,
					  .len = 1,
					  .buf = &reg,
				  },
				  {
					  .addr = client->addr,
					  .flags = I2C_M_RD,
					  .len = len,
					  .buf = buf,
				  } };

	if (i2c_transfer(client->adapter, msg, 2) != 2) {
		dev_err(&client->dev,
			"adv_wdt_i2c_read: i2c transfer failed\n");
		return -EIO;
	}

	msleep(10);
	return 0;
}

static int adv_wdt_i2c_set_timeout_2_bytes(struct i2c_client *client,
					   unsigned int val)
{
	int ret = 0;
	val = WDOG_SEC_TO_COUNT(val) & 0x0000FFFF;
	ret = adv_wdt_i2c_write_reg(client, REG_WDT_WATCHDOG_TIME_OUT, &val, 2);
	adv_wdt_i2c_read_reg(client, REG_WDT_WATCHDOG_TIME_OUT, &val,
			     sizeof(val));

	if (ret)
		return -EIO;
	return 0;
}

static int adv_wdt_i2c_set_timeout(struct i2c_client *client, unsigned int val)
{
	int ret = 0;
	val = WDOG_SEC_TO_COUNT(val) & 0x0000FFFF;
	ret = adv_wdt_i2c_write_reg(client, REG_WDT_WATCHDOG_TIME_OUT, &val,
				    sizeof(val));
	adv_wdt_i2c_read_reg(client, REG_WDT_WATCHDOG_TIME_OUT, &val,
			     sizeof(val));
	if (ret)
		return -EIO;
	return 0;
}

static int adv_wdt_i2c_read_timeout(struct i2c_client *client,
				    unsigned int *val)
{
	int ret = 0;

	ret = adv_wdt_i2c_read_reg(client, REG_WDT_WATCHDOG_TIME_OUT, val,
				   sizeof(*val));

	if (ret)
		return -EIO;
	*val = WDOG_COUNT_TO_SEC(*val & 0x0000FFFF);
	return 0;
}

static int adv_wdt_i2c_read_remain_time(struct i2c_client *client,
					unsigned int *val)
{
	int ret = 0;

	ret = adv_wdt_i2c_read_reg(client, REG_WDT_REMAIN_TIME_OUT, val,
				   sizeof(*val));
	if (ret)
		return -EIO;
	return 0;
}

static int adv_wdt_i2c_read_version(struct i2c_client *client,
				    unsigned int *val)
{
	int ret = 0;

	ret = adv_wdt_i2c_read_reg(client, REG_WDT_VERSION, val, sizeof(*val));
	if (ret)
		return -EIO;
	return 0;
}

static inline void adv_wdt_ping(void)
{
	adv_wdt.wdt_ping_status = !adv_wdt.wdt_ping_status;
	// gpio_set_value(adv_wdt.gpio_wdt_ping, adv_wdt.wdt_ping_status);
    if (adv_wdt.gpiod_wdt_ping)
    {
        gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_ping,
                                 adv_wdt.wdt_ping_status);
    }
}

static void adv_wdt_start(void)
{
	// if (!test_and_set_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
	// 	/* at our first start we enable clock and do initialisations */
	// 	gpio_set_value(adv_wdt.gpio_wdt_en, !adv_wdt.wdt_en_off);
	// }

	// /* Watchdog is enabled - time to reload the timeout value */
	// adv_wdt_ping();

    if (!test_and_set_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
        /* First start: enable watchdog */
        if (adv_wdt.gpiod_wdt_en)
            gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_en, 1);
    }

    /* Feed once after start */
    adv_wdt_ping();
}

static void adv_wdt_stop(void)
{
	// adv_wdt_ping();

	// /* we don't need a clk_disable, it cannot be disabled once started.
	//  * We use a timer to ping the watchdog while /dev/watchdog is closed */
	// gpio_set_value(adv_wdt.gpio_wdt_en, adv_wdt.wdt_en_off);

    adv_wdt_ping();

    /* Disable watchdog (or enter the defined off state) */
    if (adv_wdt.gpiod_wdt_en)
        gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_en, 0);
}

static int adv_wdt_open(struct inode *inode, struct file *file)
{
	if (test_and_set_bit(ADV_WDT_STATUS_OPEN, &adv_wdt.status))
		return -EBUSY;
	adv_wdt_start();
	return nonseekable_open(inode, file);
}

static int adv_wdt_close(struct inode *inode, struct file *file)
{
	if (test_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status) && !nowayout)
		adv_wdt_stop();
	else {
		dev_crit(adv_wdt_miscdev.parent,
			 "Unexpected close: Expect reboot!\n");
		adv_wdt_ping();
	}

	clear_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	clear_bit(ADV_WDT_STATUS_OPEN, &adv_wdt.status);
	clear_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status);
	return 0;
}

static long adv_wdt_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	unsigned int new_value = 0;
	int ret = 0;

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		adv_wdt_ping();
		return copy_to_user(argp, &adv_wdt_info,
				    sizeof(struct watchdog_info)) ?
			       -EFAULT :
			       0;

	case WDIOC_GETSTATUS:
		return put_user(adv_wdt.status, p);

	case WDIOC_KEEPALIVE:
		adv_wdt_ping();
		return 0;

	case WDIOC_SETTIMEOUT:
		if (get_user(new_value, p))
			return -EFAULT;

		if ((new_value < 1) || (new_value > ADV_WDT_MAX_TIME)) {
			pr_err("timeout value must be between 1 and %d\n",
			       ADV_WDT_MAX_TIME);
			return -EINVAL;
		}
		adv_wdt.timeout = new_value;
		ret = adv_wdt_i2c_set_timeout(adv_client, adv_wdt.timeout);
		adv_wdt_ping();
		return ret;
		/* Fallthrough to return current value */
	case WDIOC_GETTIMEOUT:
		adv_wdt_i2c_read_timeout(adv_client, &adv_wdt.timeout);
		return put_user(adv_wdt.timeout & 0xFFFF, p);

	case WDIOC_GETTIMELEFT:
		adv_wdt_i2c_read_remain_time(adv_client, &adv_wdt.remain_time);
		return put_user(adv_wdt.remain_time & 0xFFFF, p);

	default:
		return -ENOTTY;
	}
}

static ssize_t adv_wdt_write(struct file *file, const char __user *data,
			     size_t len, loff_t *ppos)
{
	size_t i;
	char c;

	if (len == 0) /* Can we see this even ? */
		return 0;

	clear_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	/* scan to see whether or not we got the magic character */
	for (i = 0; i != len; i++) {
		if (get_user(c, data + i))
			return -EFAULT;
		if (c == 'V')
			set_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	}

	adv_wdt_ping();

	return len;
}

static const struct file_operations adv_wdt_fops = {
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
	.unlocked_ioctl = adv_wdt_ioctl,
	.open = adv_wdt_open,
	.release = adv_wdt_close,
	.write = adv_wdt_write,
};

static struct miscdevice adv_wdt_miscdev = {
	//.minor = WATCHDOG_MINOR,
    .minor = 0,
	.name = "watchdog1",
	.fops = &adv_wdt_fops,
};

// static int adv_wdt_i2c_probe(struct i2c_client *client,
// 			     const struct i2c_device_id *id)
static int adv_wdt_i2c_probe(struct i2c_client *client)
{
	int ret;
	unsigned int tmp_version;
	struct device_node *np = client->dev.of_node;
	//enum of_gpio_flags flags;

	//dev_err(&client->dev, "in probe111133: %d\n", ret);

	if (!np) {
		return -ENODEV;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		return -ENODEV;
	}

	i2c_set_clientdata(client, &adv_wdt);

	adv_client = client;
	//Setting GPIO
	// adv_wdt.gpio_wdt_en = of_get_named_gpio_flags(np, "wdt-en", 0, &flags);
	// if (!gpio_is_valid(adv_wdt.gpio_wdt_en))
	// 	return -ENODEV;
	// adv_wdt.wdt_en_off = flags;
	// ret = devm_gpio_request_one(&client->dev, adv_wdt.gpio_wdt_en,
	// 			GPIOF_OUT_INIT_LOW, "adv_wdt.wdt_en");
	// if (ret < 0) {
	// 	dev_err(&client->dev, "request gpio failed: %d\n", ret);
	// 	return ret;
	// }
	// gpio_direction_output(adv_wdt.gpio_wdt_en, adv_wdt.wdt_en_off);

	// adv_wdt.gpio_wdt_ping = of_get_named_gpio_flags(np, "wdt-ping", 0, &flags);
	// if (!gpio_is_valid(adv_wdt.gpio_wdt_ping))
	// 	return -ENODEV;

	// ret = devm_gpio_request_one(&client->dev, adv_wdt.gpio_wdt_ping,
	// 			GPIOF_OUT_INIT_LOW, "adv_wdt.wdt_ping");
	// if (ret < 0) {
	// 	dev_err(&client->dev, "request gpio failed: %d\n", ret);
	// 	return ret;
	// }

	// msleep(200);
	// gpio_direction_output(adv_wdt.gpio_wdt_ping, 1);
	// adv_wdt.wdt_ping_status=1;

	/* Setting GPIO via gpiod */
	adv_wdt.gpiod_wdt_en =
		devm_gpiod_get(&client->dev, "wdt-en", GPIOD_OUT_LOW);
	if (IS_ERR(adv_wdt.gpiod_wdt_en))
		return dev_err_probe(&client->dev,
				     PTR_ERR(adv_wdt.gpiod_wdt_en),
				     "get wdt-en gpio failed\n");

	adv_wdt.gpiod_wdt_ping =
		devm_gpiod_get(&client->dev, "wdt-ping", GPIOD_OUT_LOW);
	if (IS_ERR(adv_wdt.gpiod_wdt_ping))
		return dev_err_probe(&client->dev,
				     PTR_ERR(adv_wdt.gpiod_wdt_ping),
				     "get wdt-ping gpio failed\n");

	msleep(200);
	gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_ping, 1);
	adv_wdt.wdt_ping_status = 1;
    adv_wdt.wdt_en_off = 0;

	adv_wdt.timeout = clamp_t(unsigned, timeout, 1, ADV_WDT_MAX_TIME);
	if (adv_wdt.timeout != timeout)
		dev_warn(&client->dev,
			 "Initial timeout out of range! "
			 "Clamped from %u to %u\n",
			 timeout, adv_wdt.timeout);

	ret = adv_wdt_i2c_read_version(client, &tmp_version);
	if (ret == 0) {
		adv_wdt.version[0] = (tmp_version & 0xFF00) >> 8;
		adv_wdt.version[1] = tmp_version & 0xFF;
		adv_wdt_info.firmware_version =
			(unsigned int)(adv_wdt.version[1] - '0') * 10 +
			(unsigned int)(adv_wdt.version[0] - '0');
	} else {
		pr_err("Read watchdog version err=%d\n", ret);
		goto fail;
	}

	//There is NAK when first time to set 4 bytes timeout.
	adv_wdt_i2c_set_timeout_2_bytes(client, adv_wdt.timeout);

	ret = adv_wdt_i2c_set_timeout(client, adv_wdt.timeout);
	if (ret) {
		pr_err("Set watchdog timeout err=%d\n", ret);
		goto fail;
	}

	adv_wdt_miscdev.parent = &client->dev;
	ret = misc_register(&adv_wdt_miscdev);
	if (ret) {
		pr_err("cannot register miscdev on minor=%d (err=%d)\n",
		       WATCHDOG_MINOR, ret);
		goto fail;
	}

	dev_info(
		&client->dev,
		"Advantech MSP430 Watchdog Timer enabled. timeout=%ds (nowayout=%d), Ver.%d\n",
		adv_wdt.timeout, nowayout, adv_wdt_info.firmware_version);

	return 0;

fail:
	adv_wdt_miscdev.parent = NULL;
	return ret;
}

//static void __exit adv_wdt_i2c_remove(struct i2c_client *client)
static void adv_wdt_i2c_remove(struct i2c_client *client)
{
	// misc_deregister(&adv_wdt_miscdev);

	// if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
	// 	gpio_set_value(adv_wdt.gpio_wdt_en, adv_wdt.wdt_en_off);
	// 	dev_crit(adv_wdt_miscdev.parent,
	// 		 "Device removed: Expect reboot!\n");
	// }
	// clear_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	// clear_bit(ADV_WDT_STATUS_OPEN, &adv_wdt.status);
	// clear_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status);
	// adv_wdt_miscdev.parent = NULL;

	// return;
    misc_deregister(&adv_wdt_miscdev);

	if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
		/* Disable watchdog enable (or enter off state) */
		if (!IS_ERR_OR_NULL(adv_wdt.gpiod_wdt_en))
			gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_en,
						 adv_wdt.wdt_en_off);

		dev_crit(&client->dev, "Device removed: Expect reboot!\n");
	}

	clear_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	clear_bit(ADV_WDT_STATUS_OPEN, &adv_wdt.status);
	clear_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status);

	adv_wdt_miscdev.parent = NULL;
}

static int adv_wdt_i2c_resume(struct device *dev)
{
	// if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
	// 	gpio_set_value(adv_wdt.gpio_wdt_en, !adv_wdt.wdt_en_off);
	// 	adv_wdt_i2c_set_timeout(adv_client, adv_wdt.timeout);
	// 	adv_wdt_ping();
	// }
	// return 0;

    if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {

		/* Re-enable after resume */
		if (!IS_ERR_OR_NULL(adv_wdt.gpiod_wdt_en))
			gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_en,
						 !adv_wdt.wdt_en_off);

		/* Re-apply timeout (ensure I2C handle exists) */
		if (adv_client)
			adv_wdt_i2c_set_timeout(adv_client, adv_wdt.timeout);

		adv_wdt_ping();
	}
	return 0;
}

static int adv_wdt_i2c_suspend(struct device *dev)
{
	if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
		adv_wdt_stop();
	}
	return 0;
}

static void adv_wdt_i2c_shutdown(struct i2c_client *client)
{
	// if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
	// 	/* we are running, we need to delete the timer but will give
	// 	 * max timeout before reboot will take place */
	// 	gpio_set_value(adv_wdt.gpio_wdt_en, adv_wdt.wdt_en_off);

	// 	dev_crit(adv_wdt_miscdev.parent,
	// 		 "Device shutdown: Expect reboot!\n");
	// }
	// clear_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	// clear_bit(ADV_WDT_STATUS_OPEN, &adv_wdt.status);
	// clear_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status);

    if (test_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status)) {
		/* Set max timeout or off state to avoid trigger during shutdown */
		if (!IS_ERR_OR_NULL(adv_wdt.gpiod_wdt_en))
			gpiod_set_value_cansleep(adv_wdt.gpiod_wdt_en,
						 adv_wdt.wdt_en_off);

		dev_crit(&client->dev, "Device shutdown: Expect reboot!\n");
	}

	clear_bit(ADV_WDT_EXPECT_CLOSE, &adv_wdt.status);
	clear_bit(ADV_WDT_STATUS_OPEN, &adv_wdt.status);
	clear_bit(ADV_WDT_STATUS_STARTED, &adv_wdt.status);
}

static const struct i2c_device_id adv_wdt_i2c_id[] = {
	{ DRIVER_NAME, 0 },
	{},
};

MODULE_DEVICE_TABLE(i2c, adv_wdt_i2c_id);

static const struct of_device_id adv_wdt_i2c_dt_ids[] = {
	{
		.compatible = "adv-wdt-i2c",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, adv_wdt_i2c_dt_ids);

static const struct dev_pm_ops adv_wdt_device_pm_ops = {
	.resume = adv_wdt_i2c_resume,
	.suspend = adv_wdt_i2c_suspend,
};

static struct i2c_driver adv_wdt_i2c_driver = {
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = adv_wdt_i2c_dt_ids,
		   .pm = &adv_wdt_device_pm_ops,
		   },
	/* 6.1: single-argument probe belongs in .probe_new; .probe still takes
	 * (client, id) until it was collapsed in 6.6. */
	.probe_new = adv_wdt_i2c_probe,
	.remove = adv_wdt_i2c_remove,
	.shutdown	= adv_wdt_i2c_shutdown,
	.id_table = adv_wdt_i2c_id,
};

static int __init adv_wdt_i2c_init(void)
{
	return i2c_add_driver(&adv_wdt_i2c_driver);
}

static void __exit adv_wdt_i2c_exit(void)
{
	i2c_del_driver(&adv_wdt_i2c_driver);
}

module_init(adv_wdt_i2c_init);
module_exit(adv_wdt_i2c_exit);

MODULE_DESCRIPTION("Advantech Watchdog I2C Driver");
MODULE_LICENSE("GPL");
