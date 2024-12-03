#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include "framebuffer.h"

struct type_name {
    unsigned int type;
    const char *name;
};

static const struct type_name connector_type_names[] = {
    { DRM_MODE_CONNECTOR_Unknown, "unknown" },
    { DRM_MODE_CONNECTOR_VGA, "VGA" },
    { DRM_MODE_CONNECTOR_DVII, "DVI-I" },
    { DRM_MODE_CONNECTOR_DVID, "DVI-D" },
    { DRM_MODE_CONNECTOR_DVIA, "DVI-A" },
    { DRM_MODE_CONNECTOR_Composite, "composite" },
    { DRM_MODE_CONNECTOR_SVIDEO, "s-video" },
    { DRM_MODE_CONNECTOR_LVDS, "LVDS" },
    { DRM_MODE_CONNECTOR_Component, "component" },
    { DRM_MODE_CONNECTOR_9PinDIN, "9-pin DIN" },
    { DRM_MODE_CONNECTOR_DisplayPort, "DP" },
    { DRM_MODE_CONNECTOR_HDMIA, "HDMI-A" },
    { DRM_MODE_CONNECTOR_HDMIB, "HDMI-B" },
    { DRM_MODE_CONNECTOR_TV, "TV" },
    { DRM_MODE_CONNECTOR_eDP, "eDP" },
    { DRM_MODE_CONNECTOR_VIRTUAL, "Virtual" },
    { DRM_MODE_CONNECTOR_DSI, "DSI" },
    { DRM_MODE_CONNECTOR_DPI, "DPI" },
};

const char *connector_type_name(unsigned int type)
{
    if (type < ARRAY_SIZE(connector_type_names) && type >= 0) {
        return connector_type_names[type].name;
    }

    return "INVALID";
}

void release_framebuffer(struct framebuffer *fb)
{
    if (fb->fd) {
        /* Try to become master again, else we can't set CRTC. Then the current master needs to reset everything. */
        drmSetMaster(fb->fd);
        if (fb->crtc) {
            /* Set back to orignal frame buffer */
            drmModeSetCrtc(fb->fd, fb->crtc->crtc_id, fb->crtc->buffer_id, 0, 0, &fb->connector->connector_id, 1, fb->resolution);
            drmModeFreeCrtc(fb->crtc);
        }
        if (fb->buffer_id)
            drmModeFreeFB(drmModeGetFB(fb->fd, fb->buffer_id));
        /* This will also release resolution */
        if (fb->connector) {
            drmModeFreeConnector(fb->connector);
            fb->resolution = 0;
        }
        if (fb->dumb_framebuffer.handle)
            ioctl(fb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, fb->dumb_framebuffer);
        close(fb->fd);
    }

}

int get_framebuffer(const char *dri_device, const char *connector_name, struct framebuffer *fb,
                    int selected_resolution)
{
    int err;
    int fd;
    drmModeResPtr res;
    drmModeEncoderPtr encoder = 0;
    uint32_t handles[4], pitches[4], offsets[4];

    /* Open the dri device /dev/dri/cardX */
    fd = open(dri_device, O_RDWR);
    if (fd < 0) {
        printf("Could not open dri device %s\n", dri_device);
        return -EINVAL;
    }

    /* Get the resources of the DRM device (connectors, encoders, etc.)*/
    res = drmModeGetResources(fd);
    if (!res) {
        printf("Could not get drm resources\n");
        return -EINVAL;
    }

    /* Search the connector provided as argument */
    drmModeConnectorPtr connector = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        char name[32];

        connector = drmModeGetConnectorCurrent(fd, res->connectors[i]);
        if (!connector)
            continue;

        snprintf(name, sizeof(name), "%s-%u", connector_type_name(connector->connector_type),
                 connector->connector_type_id);

        if (strncmp(name, connector_name, sizeof(name)) == 0)
            break;

        drmModeFreeConnector(connector);
        connector = 0;
    }

    if (!connector) {
        printf("Could not find matching connector %s\n", connector_name);
        return -EINVAL;
    }

    if (connector->count_modes <= 0) {
        printf("No modes found for connector %s\n", connector_name);
        return -EINVAL;
    }

    /* Get the resolution */
    drmModeModeInfoPtr resolution = 0;
    if (selected_resolution >= 0 && selected_resolution < connector->count_modes) {
        resolution = &connector->modes[selected_resolution];
    }
    else {
        for (int i = 0; i < connector->count_modes; i++) {
            drmModeModeInfoPtr res = 0;
            res = &connector->modes[i];
            if (res->type & DRM_MODE_TYPE_PREFERRED)
                resolution = res;
        }
    }
    if (!resolution) {
        printf("Could not find preferred resolution, use first possible resolution\n");
        resolution = &connector->modes[0];
    }

    fb->dumb_framebuffer.height = resolution->vdisplay;
    fb->dumb_framebuffer.width = resolution->hdisplay;
    fb->dumb_framebuffer.bpp = 32;

    err = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &fb->dumb_framebuffer);
    if (err) {
        printf("Could not create dumb framebuffer (err=%d)\n", err);
        goto cleanup;
    }

    handles[0] = fb->dumb_framebuffer.handle;
    pitches[0] = fb->dumb_framebuffer.pitch;
    offsets[0] = 0;
    err = drmModeAddFB2(fd, resolution->hdisplay, resolution->vdisplay, DRM_FORMAT_ABGR8888,
                        handles, pitches, offsets, &fb->buffer_id, 0);
    if (err) {
        printf("Could not add framebuffer to drm (err=%d)\n", err);
        goto cleanup;
    }

    encoder = drmModeGetEncoder(fd, connector->encoder_id ? connector->encoder_id : connector->encoders[0]);
    if (!encoder) {
        printf("Could not get encoder\n");
        err = -EINVAL;
        goto cleanup;
    }

    /* Get the crtc settings */
    fb->crtc = drmModeGetCrtc(fd, encoder->crtc_id ? encoder->crtc_id : res->crtcs[0]);
    if (!fb->crtc) {
        printf("Could not get crtc\n");
        err = -EINVAL;
        goto cleanup;
    }

    struct drm_mode_map_dumb mreq;

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = fb->dumb_framebuffer.handle;

    err = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (err) {
        printf("Mode map dumb framebuffer failed (err=%d)\n", err);
        goto cleanup;
    }

    fb->data = mmap(0, fb->dumb_framebuffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (fb->data == MAP_FAILED) {
        err = errno;
        printf("Mode map failed (err=%d)\n", err);
        goto cleanup;
    }

    /* Make sure we are not master anymore so that other processes can add new framebuffers as well */
    drmDropMaster(fd);

    fb->fd = fd;
    fb->connector = connector;
    fb->resolution = resolution;

cleanup:
    /* We don't need the encoder and connector anymore so let's free them */
    if (encoder)
        drmModeFreeEncoder(encoder);

    if (err)
        release_framebuffer(fb);

    return err;
}
