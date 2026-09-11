#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/version.h>

#include "board-name.h"

#define RESET_DELAY				100 // 100ms

static char board_name[32] = { 0 };

static int board_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s", board_name);
	return 0;
}

static int board_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, board_proc_show, NULL);
}

static const struct proc_ops board_proc_fops = {
	.proc_open		= board_proc_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

static int __init proc_board_init(void)
{
	proc_create("board", 0, NULL, &board_proc_fops);
	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0))
#include "../gpio/gpiolib.h"

enum of_gpio_flags {
	OF_GPIO_ACTIVE_LOW = 0x1,
	OF_GPIO_SINGLE_ENDED = 0x2,
	OF_GPIO_OPEN_DRAIN = 0x4,
	OF_GPIO_TRANSITORY = 0x8,
	OF_GPIO_PULL_UP = 0x10,
	OF_GPIO_PULL_DOWN = 0x20,
	OF_GPIO_PULL_DISABLE = 0x40,
};

static int of_gpiochip_match_node_and_xlate(struct gpio_chip *chip, const void *data)
{
	const struct of_phandle_args *gpiospec = data;

	return device_match_of_node(&chip->gpiodev->dev, gpiospec->np) &&
				chip->of_xlate &&
				chip->of_xlate(chip, gpiospec, NULL) >= 0;
}

static struct gpio_device *of_find_gpio_device_by_xlate(const struct of_phandle_args *gpiospec)
{
	return gpio_device_find(gpiospec, of_gpiochip_match_node_and_xlate);
}

static struct gpio_desc *of_xlate_and_get_gpiod_flags(struct gpio_chip *chip,
					struct of_phandle_args *gpiospec,
					enum of_gpio_flags *flags)
{
	int ret;

	if (chip->of_gpio_n_cells != gpiospec->args_count)
		return ERR_PTR(-EINVAL);

	ret = chip->of_xlate(chip, gpiospec, flags);
	if (ret < 0)
		return ERR_PTR(ret);

	return gpiochip_get_desc(chip, ret);
}

static struct gpio_desc *of_get_named_gpiod_flags(const struct device_node *np,
		     const char *propname, int index, enum of_gpio_flags *flags)
{
	struct of_phandle_args gpiospec;
	struct gpio_desc *desc;
	int ret;

	ret = of_parse_phandle_with_args_map(np, propname, "gpio", index,
					     &gpiospec);
	if (ret) {
		pr_debug("%s: can't parse '%s' property of node '%pOF[%d]'\n",
			__func__, propname, np, index);
		return ERR_PTR(ret);
	}

	struct gpio_device *gdev __free(gpio_device_put) =
				of_find_gpio_device_by_xlate(&gpiospec);
	if (!gdev) {
		desc = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	desc = of_xlate_and_get_gpiod_flags(gpio_device_get_chip(gdev),
					    &gpiospec, flags);
	if (IS_ERR(desc))
		goto out;

	pr_debug("%s: parsed '%s' property of node '%pOF[%d]' - status (%d)\n",
		 __func__, propname, np, index,
		 PTR_ERR_OR_ZERO(desc));

out:
	of_node_put(gpiospec.np);

	return desc;
}

static int of_get_named_gpio_flags(const struct device_node *np, const char *list_name,
			    int index, enum of_gpio_flags *flags)
{
	struct gpio_desc *desc;

	desc = of_get_named_gpiod_flags(np, list_name, index, flags);

	if (IS_ERR(desc))
		return PTR_ERR(desc);
	else
		return desc_to_gpio(desc);
}
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))
#include "../gpio/gpiolib.h"

enum of_gpio_flags {
	OF_GPIO_ACTIVE_LOW = 0x1,
	OF_GPIO_SINGLE_ENDED = 0x2,
	OF_GPIO_OPEN_DRAIN = 0x4,
	OF_GPIO_TRANSITORY = 0x8,
	OF_GPIO_PULL_UP = 0x10,
	OF_GPIO_PULL_DOWN = 0x20,
	OF_GPIO_PULL_DISABLE = 0x40,
};

static int of_gpiochip_match_node_and_xlate(struct gpio_chip *chip, void *data)
{
	struct of_phandle_args *gpiospec = data;

	return device_match_of_node(&chip->gpiodev->dev, gpiospec->np) &&
				chip->of_xlate &&
				chip->of_xlate(chip, gpiospec, NULL) >= 0;
}

static struct gpio_chip *of_find_gpiochip_by_xlate(
					struct of_phandle_args *gpiospec)
{
	return gpiochip_find(gpiospec, of_gpiochip_match_node_and_xlate);
}

static struct gpio_desc *of_xlate_and_get_gpiod_flags(struct gpio_chip *chip,
					struct of_phandle_args *gpiospec,
					enum of_gpio_flags *flags)
{
	int ret;

	if (chip->of_gpio_n_cells != gpiospec->args_count)
		return ERR_PTR(-EINVAL);

	ret = chip->of_xlate(chip, gpiospec, flags);
	if (ret < 0)
		return ERR_PTR(ret);

	return gpiochip_get_desc(chip, ret);
}

static struct gpio_desc *of_get_named_gpiod_flags(const struct device_node *np,
		     const char *propname, int index, enum of_gpio_flags *flags)
{
	struct of_phandle_args gpiospec;
	struct gpio_chip *chip;
	struct gpio_desc *desc;
	int ret;

	ret = of_parse_phandle_with_args_map(np, propname, "gpio", index,
					     &gpiospec);
	if (ret) {
		pr_debug("%s: can't parse '%s' property of node '%pOF[%d]'\n",
			__func__, propname, np, index);
		return ERR_PTR(ret);
	}

	chip = of_find_gpiochip_by_xlate(&gpiospec);
	if (!chip) {
		desc = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	desc = of_xlate_and_get_gpiod_flags(chip, &gpiospec, flags);
	if (IS_ERR(desc))
		goto out;

	pr_debug("%s: parsed '%s' property of node '%pOF[%d]' - status (%d)\n",
		 __func__, propname, np, index,
		 PTR_ERR_OR_ZERO(desc));

out:
	of_node_put(gpiospec.np);

	return desc;
}

static int of_get_named_gpio_flags(const struct device_node *np, const char *list_name,
			    int index, enum of_gpio_flags *flags)
{
	struct gpio_desc *desc;

	desc = of_get_named_gpiod_flags(np, list_name, index, flags);

	if (IS_ERR(desc))
		return PTR_ERR(desc);
	else
		return desc_to_gpio(desc);
}
#endif

static int adv_board_probe(struct platform_device *pdev)
{
	const char *bd_name;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	bool active;
	int i, gpio;
	int num_en_gpios, num_reset_gpios;
	int reset_delay = RESET_DELAY;
	enum of_gpio_flags flags;

	if (!np) {
		dev_err(dev, "No device tree node found\n");
		return -ENODEV;
	}

	if (of_property_read_string(np, "board_name", &bd_name)) {
		dev_err(dev, "Failed to read bd_name from device tree\n");
		return -EINVAL;
	}

	snprintf(board_name, sizeof(board_name), "%s\n", bd_name); 
	printk("+++%s init.\n", board_name);
	proc_board_init();
	dev_info(dev, "board name: %s\n", bd_name);
	
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))
	num_en_gpios = of_count_phandle_with_args(np, "en-gpios", "#gpio-cells");
	num_reset_gpios = of_count_phandle_with_args(np, "reset-gpios", "#gpio-cells");
#else
    num_en_gpios = of_gpio_named_count(np, "en-gpios");
    num_reset_gpios = of_gpio_named_count(np, "reset-gpios");
#endif
	
    // en-gpios
    for (i = 0; i < num_en_gpios; i++)
    {
        gpio = of_get_named_gpio_flags(np, "en-gpios", i, &flags);
        if (gpio_is_valid(gpio))
        {
			if (!gpio_request(gpio, "en_gpios"))
            {
                active = !(flags & OF_GPIO_ACTIVE_LOW);
                if (active)
                	gpio_direction_output(gpio, GPIOF_OUT_INIT_HIGH);
                else
                    gpio_direction_output(gpio, GPIOF_OUT_INIT_LOW);
            }
            gpio_free(gpio);
        }
    }

    // reset-gpios
    for (i = 0; i < num_reset_gpios; i++)
    {
        gpio = of_get_named_gpio_flags(np, "reset-gpios", i, &flags);
        if (gpio_is_valid(gpio))
        {
            if (!gpio_request(gpio, "reset_gpios"))
            {
                active = !(flags & OF_GPIO_ACTIVE_LOW);
                if (active)
                    gpio_direction_output(gpio, GPIOF_OUT_INIT_HIGH);
                else
                    gpio_direction_output(gpio, GPIOF_OUT_INIT_LOW);
            }
        }
    }

    // reset-delay
    if (of_property_read_u32(np, "reset-delay", &reset_delay))
        reset_delay = RESET_DELAY;
    if (reset_delay)
        mdelay(reset_delay);

    for (i = 0; i < num_reset_gpios; i++)
    {
        gpio = of_get_named_gpio_flags(np, "reset-gpios", i, &flags);
        if (gpio_is_valid(gpio))
        {
            active = !(flags & OF_GPIO_ACTIVE_LOW);
            if (active)
                gpio_direction_output(gpio, GPIOF_OUT_INIT_LOW);
            else
                gpio_direction_output(gpio, GPIOF_OUT_INIT_HIGH);
            gpio_free(gpio);
        }
    }

	return 0;
}

static const struct of_device_id adv_board_of_match[] = {
	{ .compatible = "custom,adv-board", },
	{},
};
MODULE_DEVICE_TABLE(of, adv_board_of_match);

static struct platform_driver adv_board_driver = {
	.probe = adv_board_probe,
	.driver = {
		.name = "adv_board",
		.of_match_table = adv_board_of_match,
	},
};

static int __init adv_board_init(void)
{
	int ret = platform_driver_register(&adv_board_driver);
	if (!ret) {
		printk(KERN_ERR "adv_board: %s\n", "Registerered");
	} else {
		printk(KERN_ERR "adv_board: %s\n", "Failed");
	}
	
	return ret;
}

static void __exit adv_board_exit(void)
{
	remove_proc_entry("board", NULL);
	platform_driver_unregister(&adv_board_driver);
}

module_init(adv_board_init);
module_exit(adv_board_exit);

MODULE_AUTHOR("Yuyan.Tsai");
MODULE_DESCRIPTION("Advantech board driver");
MODULE_LICENSE("GPL v2");

