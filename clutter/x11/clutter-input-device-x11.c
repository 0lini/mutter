#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-input-device-x11.h"
#include "../clutter-debug.h"
#include "../clutter-private.h"

#ifdef HAVE_XINPUT
#include <X11/extensions/XInput.h>
#endif

typedef struct _ClutterInputDeviceClass         ClutterInputDeviceX11Class;

/* a specific X11 input device */
struct _ClutterInputDeviceX11
{
  ClutterInputDevice device;

#ifdef HAVE_XINPUT
  XDevice           *xdevice;
  XEventClass        xevent_list[5];   /* MAX 5 event types */
  int                num_events;
#endif

  guint is_core : 1;
};

enum
{
  PROP_0,

  PROP_IS_CORE
};

G_DEFINE_TYPE (ClutterInputDeviceX11,
               clutter_input_device_x11,
               CLUTTER_TYPE_INPUT_DEVICE);

static void
clutter_input_device_x11_set_property (GObject      *gobject,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ClutterInputDeviceX11 *self = CLUTTER_INPUT_DEVICE_X11 (gobject);

  switch (prop_id)
    {
    case PROP_IS_CORE:
      self->is_core = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_input_device_x11_get_property (GObject    *gobject,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ClutterInputDeviceX11 *self = CLUTTER_INPUT_DEVICE_X11 (gobject);

  switch (prop_id)
    {
    case PROP_IS_CORE:
      g_value_set_boolean (value, self->is_core);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_input_device_x11_class_init (ClutterInputDeviceX11Class *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  gobject_class->set_property = clutter_input_device_x11_set_property;
  gobject_class->get_property = clutter_input_device_x11_get_property;

  pspec = g_param_spec_boolean ("is-core",
                                "Is Core",
                                "Whether the device is a core one",
                                FALSE,
                                CLUTTER_PARAM_READWRITE |
                                G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (gobject_class, PROP_IS_CORE, pspec);
}

static void
clutter_input_device_x11_init (ClutterInputDeviceX11 *self)
{
  self->is_core = FALSE;
}

gint
_clutter_input_device_x11_construct (ClutterInputDevice *device,
                                     ClutterBackendX11  *backend)
{
  int n_events = 0;

#ifdef HAVE_XINPUT
  ClutterInputDeviceX11 *device_x11;
  XDevice *x_device = NULL;
  gint device_id;
  int i;

  device_x11 = CLUTTER_INPUT_DEVICE_X11 (device);
  device_id = clutter_input_device_get_device_id (device);

  clutter_x11_trap_x_errors ();

  /* retrieve the X11 device */
  x_device = XOpenDevice (backend->xdpy, device_id);

  if (clutter_x11_untrap_x_errors () || x_device == NULL)
    {
      CLUTTER_NOTE (BACKEND, "Unable to open device %i", device_id);
      return 0;
    }

  device_x11->xdevice = x_device;

  CLUTTER_NOTE (BACKEND,
                "Registering XINPUT device with XID: %li",
                x_device->device_id);

  /* We must go through all the classes supported by this device and
   * register the appropriate events we want. Each class only appears
   * once. We need to store the types with the stage since they are
   * created dynamically by the server. They are not device specific.
   */
  for (i = 0; i < x_device->num_classes; i++)
    {
      XInputClassInfo *xclass_info = x_device->classes + i;
      int *button_press, *button_release, *motion_notify;
      int *key_press, *key_release;

      button_press =
        &backend->event_types[CLUTTER_X11_XINPUT_BUTTON_PRESS_EVENT];
      button_release =
        &backend->event_types[CLUTTER_X11_XINPUT_BUTTON_RELEASE_EVENT];
      motion_notify =
        &backend->event_types[CLUTTER_X11_XINPUT_MOTION_NOTIFY_EVENT];

      key_press =
        &backend->event_types[CLUTTER_X11_XINPUT_KEY_PRESS_EVENT];
      key_release =
        &backend->event_types[CLUTTER_X11_XINPUT_KEY_RELEASE_EVENT];

      switch (xclass_info->input_class)
        {
        /* event though XInput 1.x is broken for keyboard-like devices
         * it might still be useful to track them down; the core keyboard
         * will handle the right events anyway
         */
        case KeyClass:
          DeviceKeyPress (x_device,
                          *key_press,
                          device_x11->xevent_list[n_events]);
          n_events++;

          DeviceKeyRelease (x_device,
                            *key_release,
                            device_x11->xevent_list[n_events]);
          n_events++;
          break;

        case ButtonClass:
          DeviceButtonPress (x_device,
                             *button_press,
                             device_x11->xevent_list[n_events]);
          n_events++;

          DeviceButtonRelease (x_device,
                               *button_release,
                               device_x11->xevent_list[n_events]);
          n_events++;
          break;

        case ValuatorClass:
          DeviceMotionNotify (x_device,
                              *motion_notify,
                              device_x11->xevent_list[n_events]);
          n_events++;
          break;
        }
    }

  device_x11->num_events = n_events;
#endif /* HAVE_XINPUT */

  return n_events;
}

void
_clutter_input_device_x11_select_events (ClutterInputDevice *device,
                                         ClutterBackendX11  *backend_x11,
                                         Window              xwin)
{
#if HAVE_XINPUT
  ClutterInputDeviceX11 *device_x11;

  device_x11 = CLUTTER_INPUT_DEVICE_X11 (device);

  if (device_x11->xdevice == None || device_x11->num_events == 0)
    return;

  XSelectExtensionEvent (backend_x11->xdpy, xwin,
                         device_x11->xevent_list,
                         device_x11->num_events);
#endif /* HAVE_XINPUT */
}
