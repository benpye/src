/*
 * Elantech i2c touchpad driver
 *
 * Copyright (c) 2018 Ben Pye <ben@curlybracket.co.uk>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/stdint.h>

#include <dev/i2c/i2cvar.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>
#include <dev/hid/hid.h>
#include <dev/hid/hidmsvar.h>

/* #define IETP_DEBUG */

#ifdef IETP_DEBUG
#define DPRINTF(x) printf x
#else
#define DPRINTF(x)
#endif

#define IETP_INPUT		0x0003
#define IETP_MAX_X_AXIS		0x0106
#define IETP_MAX_Y_AXIS		0x0107
#define IETP_RESOLUTION		0x0108

#define IETP_COMMAND		0x0005
#define IETP_CONTROL		0x0300

#define IETP_CMD_WAKEUP		0x0800
#define IETP_CMD_SLEEP		0x0801
#define IETP_CMD_RESET		0x0100

#define IETP_CTRL_ABSOLUTE	0x0001
#define IETP_CTRL_STANDARD	0x0000

#define IETP_MAX_REPORT_LEN	34
#define IETP_MAX_FINGERS	5

#define IETP_REPORT_ABSOLUTE	0x5D

#define IETP_REPORT_ID		2
#define IETP_TOUCH_INFO		3
#define IETP_FINGER_DATA	4

#define IETP_TOUCH_LMB		(1 << 0)
#define IETP_TOUCH_RMB		(1 << 1)
#define IETP_TOUCH_MMB		(1 << 2)

#define IETP_FINGER_DATA_LEN	5
#define IETP_FINGER_XY_HIGH	0
#define IETP_FINGER_X_LOW	1
#define IETP_FINGER_Y_LOW	2
#define IETP_FINGER_WIDTH	3
#define IETP_FINGER_PRESSURE	4

struct ietp_softc {
	struct device		sc_dev;
	i2c_tag_t		sc_tag;

	i2c_addr_t		sc_addr;
	void			*sc_ih;

	struct device		*sc_wsmousedev;
	char			sc_hid[16];
	int			sc_enabled;
	int			sc_busy;
	struct tsscale		sc_tsscale;

	uint16_t		max_x;
	uint16_t		max_y;
	uint8_t			res_x;
	uint8_t			res_y;
};

int	ietp_match(struct device *, void *, void *);
void	ietp_attach(struct device *, struct device *, void *);
int	ietp_detach(struct device *, int);
int	ietp_activate(struct device *, int);

int	ietp_ioctl(void *, u_long, caddr_t, int, struct proc *);
int	ietp_enable(void *);
void	ietp_disable(void *);

int	ietp_read_reg(struct ietp_softc *, uint16_t, size_t, void *);
int	ietp_write_reg(struct ietp_softc *, uint16_t, uint16_t);
void	ietp_proc_report(struct ietp_softc *);
int	ietp_init(struct ietp_softc *);
int	ietp_sleep(struct ietp_softc *);
int	ietp_reset(struct ietp_softc *);
int	ietp_intr(void *);

const struct wsmouse_accessops ietp_accessops = {
	ietp_enable,
	ietp_ioctl,
	ietp_disable,
};

struct cfattach ietp_ca = {
	sizeof(struct ietp_softc),
	ietp_match,
	ietp_attach,
	ietp_detach,
	ietp_activate,
};

struct cfdriver ietp_cd = {
	NULL, "ietp", DV_DULL
};

int
ietp_match(struct device *parent, void *match, void *aux)
{
	struct i2c_attach_args *ia = aux;

	if (strcmp(ia->ia_name, "ietp") == 0)
		return 1;

	return 0;
}

void
ietp_attach(struct device *parent, struct device *self, void *aux)
{
	struct ietp_softc *sc = (struct ietp_softc *)self;
	struct i2c_attach_args *ia = aux;
	struct wsmousedev_attach_args wsmaa;

	sc->sc_tag = ia->ia_tag;
	sc->sc_addr = ia->ia_addr;

	if (ia->ia_cookie != NULL)
		memcpy(&sc->sc_hid, ia->ia_cookie, sizeof(sc->sc_hid));

	if (!ietp_init(sc))
		return;

	if (ia->ia_intr) {
		printf(" %s", iic_intr_string(sc->sc_tag, ia->ia_intr));

		sc->sc_ih = iic_intr_establish(sc->sc_tag, ia->ia_intr,
		    IPL_TTY, ietp_intr, sc, sc->sc_dev.dv_xname);
		if (sc->sc_ih == NULL) {
			printf(", can't establish interrupt\n");
			return;
		}
	}

	printf(": Elantech Touchpad (%dx%d)\n", sc->max_x, sc->max_y);

	wsmaa.accessops = &ietp_accessops;
	wsmaa.accesscookie = sc;
	sc->sc_wsmousedev = config_found(self, &wsmaa, wsmousedevprint);
}

int
ietp_detach(struct device *self, int flags)
{
	struct ietp_softc *sc = (struct ietp_softc *)self;

	if (sc->sc_ih != NULL) {
		intr_disestablish(sc->sc_ih);
		sc->sc_ih = NULL;
	}

	sc->sc_enabled = 0;

	return 0;
}

int
ietp_activate(struct device *self, int act)
{
	struct ietp_softc *sc = (struct ietp_softc *)self;

	switch(act) {
	case DVACT_QUIESCE:
		ietp_sleep(sc);
		break;
	case DVACT_WAKEUP:
		sc->sc_busy = 1;
		ietp_init(sc);
		sc->sc_busy = 0;
		break;
	}

	config_activate_children(self, act);

	return 0;
}

int
ietp_configure(struct ietp_softc *sc)
{
	struct wsmousehw *hw;

	hw = wsmouse_get_hw(sc->sc_wsmousedev);
	hw->type = WSMOUSE_TYPE_TOUCHPAD;
	hw->hw_type = WSMOUSEHW_CLICKPAD;
	hw->x_min = sc->sc_tsscale.minx;
	hw->x_max = sc->sc_tsscale.maxx;
	hw->y_min = sc->sc_tsscale.miny;
	hw->y_max = sc->sc_tsscale.maxy;
	hw->h_res = sc->sc_tsscale.resx;
	hw->v_res = sc->sc_tsscale.resy;
	hw->mt_slots = IETP_MAX_FINGERS;

	return (wsmouse_configure(sc->sc_wsmousedev, NULL, 0));
}

int
ietp_enable(void *v)
{
	struct ietp_softc *sc = v;

	if (sc->sc_busy && tsleep(&sc->sc_busy, PRIBIO, "ietp", hz) != 0) {
		printf("%s: trying to enable but we're busy\n",
		    sc->sc_dev.dv_xname);
		return 1;
	}

	sc->sc_busy = 1;

	DPRINTF(("%s: enabling\n", sc->sc_dev.dv_xname));

	if (ietp_configure(sc)) {
		printf("%s: failed wsmouse_configure\n", sc->sc_dev.dv_xname);
		return 1;
	}

	sc->sc_enabled = 1;
	sc->sc_busy = 0;

	return 0;
}

void
ietp_disable(void *v)
{
	struct ietp_softc *sc = v;

	DPRINTF(("%s: disabling\n", sc->sc_dev.dv_xname));

	wsmouse_set_mode(sc->sc_wsmousedev, WSMOUSE_COMPAT);

	sc->sc_enabled = 0;
}

int
ietp_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct ietp_softc *sc = v;
	struct wsmouse_calibcoords *wsmc = (struct wsmouse_calibcoords *)data;
	int wsmode;

	DPRINTF(("%s: %s: cmd %ld\n", sc->sc_dev.dv_xname, __func__, cmd));

	switch (cmd) {
	case WSMOUSEIO_SCALIBCOORDS:
		sc->sc_tsscale.minx = wsmc->minx;
		sc->sc_tsscale.maxx = wsmc->maxx;
		sc->sc_tsscale.miny = wsmc->miny;
		sc->sc_tsscale.maxy = wsmc->maxy;
		sc->sc_tsscale.swapxy = wsmc->swapxy;
		sc->sc_tsscale.resx = wsmc->resx;
		sc->sc_tsscale.resy = wsmc->resy;
		break;

	case WSMOUSEIO_GCALIBCOORDS:
		wsmc->minx = sc->sc_tsscale.minx;
		wsmc->maxx = sc->sc_tsscale.maxx;
		wsmc->miny = sc->sc_tsscale.miny;
		wsmc->maxy = sc->sc_tsscale.maxy;
		wsmc->swapxy = sc->sc_tsscale.swapxy;
		wsmc->resx = sc->sc_tsscale.resx;
		wsmc->resy = sc->sc_tsscale.resy;
		break;

	case WSMOUSEIO_GTYPE: {
		struct wsmousehw *hw = wsmouse_get_hw(sc->sc_wsmousedev);
		*(u_int *)data = hw->type;
		break;
	}

	case WSMOUSEIO_SETMODE:
		wsmode = *(u_int *)data;
		if (wsmode != WSMOUSE_COMPAT && wsmode != WSMOUSE_NATIVE) {
			printf("%s: invalid mode %d\n", sc->sc_dev.dv_xname,
			    wsmode);
			return EINVAL;
		}
		wsmouse_set_mode(sc->sc_wsmousedev, wsmode);
		break;

	default:
		return -1;
	}

	return 0;
}

int
ietp_reset(struct ietp_softc *sc)
{
	uint16_t buf;

	if (ietp_write_reg(sc, IETP_COMMAND, IETP_CMD_RESET)) {
		printf("%s: failed writing reset command\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	if (ietp_read_reg(sc, 0x0000, sizeof(buf), &buf)) {
		printf("%s: failed reading reset ack\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	if (ietp_write_reg(sc, IETP_CONTROL, IETP_CTRL_ABSOLUTE)) {
		printf("%s: failed setting absolute mode\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	if (ietp_write_reg(sc, IETP_COMMAND, IETP_CMD_WAKEUP)) {
		printf("%s: failed writing wakeup command\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	return 1;
}

int
ietp_sleep(struct ietp_softc *sc)
{
	if (ietp_write_reg(sc, IETP_COMMAND, IETP_CMD_SLEEP)) {
		printf("%s: failed writing sleep command\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	return 1;
}

int
ietp_init(struct ietp_softc *sc)
{
	uint16_t buf;

	sc->sc_enabled = 0;

	if (!ietp_reset(sc)) {
		printf("%s: failed to reset\n", sc->sc_dev.dv_xname);
		return 0;
	}

	if (ietp_read_reg(sc, IETP_MAX_X_AXIS, sizeof(buf), &buf)) {
		printf("%s: failed reading max x\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	sc->max_x = le16toh(buf) & 0xFFF;

	if (ietp_read_reg(sc, IETP_MAX_Y_AXIS, sizeof(buf), &buf)) {
		printf("%s: failed reading max y\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	sc->max_y = le16toh(buf) & 0xFFF;

	if (ietp_read_reg(sc, IETP_RESOLUTION, sizeof(buf), &buf)) {
		printf("%s: failed reading resolution\n",
		    sc->sc_dev.dv_xname);
		return 0;
	}

	sc->res_x = le16toh(buf) & 0xFF;
	sc->res_y = (le16toh(buf) >> 8) & 0xFF;

	/* Conversion from internal format to DPI */
	sc->res_x = 790 + sc->res_x * 10;
	sc->res_y = 790 + sc->res_y * 10;

	sc->sc_tsscale.minx = 0;
	sc->sc_tsscale.maxx = sc->max_x;
	sc->sc_tsscale.miny = 0;
	sc->sc_tsscale.maxy = sc->max_y;
	sc->sc_tsscale.swapxy = 0;
	sc->sc_tsscale.resx = sc->res_x;
	sc->sc_tsscale.resy = sc->res_y;

	return 1;
}

int
ietp_read_reg(struct ietp_softc *sc, uint16_t reg, size_t len, void *val)
{
	uint8_t cmd[2] = { reg & 0xff, (reg >> 8) & 0xff };
	int ret;

	iic_acquire_bus(sc->sc_tag, I2C_F_POLL);

	ret = iic_exec(sc->sc_tag, I2C_OP_READ_WITH_STOP, sc->sc_addr, &cmd,
	    sizeof(cmd), val, len, I2C_F_POLL);

	iic_release_bus(sc->sc_tag, I2C_F_POLL);

	return ret;
}

int
ietp_write_reg(struct ietp_softc *sc, uint16_t reg, uint16_t val)
{
	uint8_t cmd[4] = { reg & 0xff, (reg >> 8) & 0xff,
			   val & 0xff, (val >> 8) & 0xff };
	int ret;

	iic_acquire_bus(sc->sc_tag, 0);

	ret = iic_exec(sc->sc_tag, I2C_OP_WRITE_WITH_STOP, sc->sc_addr, &cmd,
	    sizeof(cmd), NULL, 0, I2C_F_POLL);

	iic_release_bus(sc->sc_tag, 0);

	return ret;
}

void
ietp_proc_report(struct ietp_softc *sc)
{
	uint8_t report[IETP_MAX_REPORT_LEN];
	uint8_t *finger_data;
	uint8_t report_id;
	uint16_t len;
	int i, s, x, y, valid_contact, pressure;
	u_int buttons;

	if (ietp_read_reg(sc, 0x00, sizeof(report), report)) {
		printf("%s: failed reading report\n",
		    sc->sc_dev.dv_xname);
		return;
	}

	report_id = report[IETP_REPORT_ID];
	len = le16toh(report[0] | (report[1] << 8));

	/* we seem to get 0 length reports sometimes, ignore them */
	if (report_id != IETP_REPORT_ABSOLUTE ||
	    len < IETP_MAX_REPORT_LEN) {
		return;
	}

	finger_data = &report[IETP_FINGER_DATA];

	for (i = 0; i < IETP_MAX_FINGERS; i++) {
		valid_contact = report[IETP_TOUCH_INFO] & (1 << (i + 3));

		x = 0;
		y = 0;
		pressure = 0;

		if (valid_contact) {
			x = (finger_data[IETP_FINGER_XY_HIGH] & 0xF0) << 4;
			y = (finger_data[IETP_FINGER_XY_HIGH] & 0x0F) << 8;

			x |= finger_data[IETP_FINGER_X_LOW];
			y |= finger_data[IETP_FINGER_Y_LOW];

			pressure = finger_data[IETP_FINGER_PRESSURE];
		}

		wsmouse_mtstate(sc->sc_wsmousedev, i, x, y, pressure);
		finger_data += IETP_FINGER_DATA_LEN;
	}

	buttons = 0;
	if (report[IETP_TOUCH_INFO] & IETP_TOUCH_LMB)
		buttons |= (1 << 0);
	if (report[IETP_TOUCH_INFO] & IETP_TOUCH_MMB)
		buttons |= (1 << 1);
	if (report[IETP_TOUCH_INFO] & IETP_TOUCH_RMB)
		buttons |= (1 << 2);

	s = spltty();

	wsmouse_buttons(sc->sc_wsmousedev, buttons);
	wsmouse_input_sync(sc->sc_wsmousedev);

	splx(s);
}

int
ietp_intr(void *arg)
{
	struct ietp_softc *sc = arg;

	if (sc->sc_busy)
		return 1;

	sc->sc_busy = 1;

	ietp_proc_report(sc);

	sc->sc_busy = 0;
	wakeup(&sc->sc_busy);

	return 1;
}
