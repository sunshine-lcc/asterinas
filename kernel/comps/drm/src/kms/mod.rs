// SPDX-License-Identifier: MPL-2.0

//! Kernel Mode Setting support for DRM devices.
//!
//! This module defines the shared KMS interface exposed by DRM drivers.
//! [`DrmKmsOps`] provides device-specific KMS operations,
//! while [`DrmModeConfig`] owns the device-wide mode configuration and synchronized KMS object store.
//!
//! Individual planes, CRTCs, encoders, connectors, and properties are defined in the [`objects`] module.

use alloc::sync::Arc;

use aster_core::prelude::*;
use ostd::sync::Mutex;

use crate::{
    device::DrmDevice,
    kms::{
        display_mode::DrmDisplayMode,
        objects::{
            DrmKmsObjectStore, KmsObjectIndex,
            connector::{DrmConnector, DrmConnectorProbeState, DrmConnectorState},
            crtc::DrmCrtcState,
            framebuffer::DrmFramebuffer,
            plane::DrmPlaneState,
        },
    },
    utils::{DrmRect, DrmSize},
};

pub mod display_info;
pub mod display_mode;
pub mod objects;
pub mod pixel_format;

/// Driver-specific operations for a KMS-capable DRM device.
///
/// A KMS-capable device exposes this interface through [`DrmDevice::as_kms_ops`].
///
/// It provides access to the device's mode configuration and implements hardware-facing operations
/// such as connector probing.
pub trait DrmKmsOps: DrmDevice {
    fn mode_config(&self) -> &DrmModeConfig;

    /// Probes a connector and returns its refreshed probe-derived state.
    ///
    /// The DRM core resolves the connector and commits the returned state.
    fn probe_connector(&self, connector: &DrmConnector) -> Result<DrmConnectorProbeState>;

    /// Configures scanout for a CRTC through the legacy modesetting path.
    ///
    /// `crtc_index` identifies the CRTC. Passing `Some(framebuffer)` enables scanout from
    /// `source_rect`; passing `None` disables scanout.
    ///
    /// The DRM core validates the mode and connector-encoder assignments before calling this
    /// method. It updates the corresponding core state only if this method returns successfully.
    ///
    /// # Locking
    ///
    /// The DRM core holds the object-store lock across this call. Implementations must not access
    /// the object store while this method is running.
    fn set_crtc(
        &self,
        crtc_index: KmsObjectIndex,
        framebuffer: Option<&DrmFramebuffer>,
        source_rect: DrmRect,
    ) -> Result<()>;

    /// Refreshes the regions of a framebuffer currently scanned out by planes.
    ///
    /// # Locking
    ///
    /// The DRM core holds the object-store lock across this call. Implementations must not access
    /// the object store while this method is running.
    fn refresh_dirty_fb(
        &self,
        framebuffer: &DrmFramebuffer,
        source_rects: &[DrmRect],
    ) -> Result<()>;
}

/// Describes a DRM device's global mode-setting capabilities and KMS objects.
#[derive(Debug)]
pub struct DrmModeConfig {
    min_fb_size: DrmSize,
    max_fb_size: DrmSize,
    cursor_size: Option<DrmSize>,
    preferred_dumb_buffer_depth: u32,

    supports_async_page_flip: bool,
    supports_fb_modifiers: bool,
    prefer_shadow_buffer: bool,

    object_store: Mutex<DrmKmsObjectStore>,
}

impl DrmModeConfig {
    pub fn new(
        min_fb_size: DrmSize,
        max_fb_size: DrmSize,
        object_store: DrmKmsObjectStore,
    ) -> Result<Self> {
        if !min_fb_size.is_within(0..=max_fb_size.width(), 0..=max_fb_size.height()) {
            return_errno_with_message!(
                Errno::EINVAL,
                "the minimum framebuffer size exceeds the maximum framebuffer size"
            );
        }

        Ok(Self {
            min_fb_size,
            max_fb_size,
            cursor_size: None,
            preferred_dumb_buffer_depth: 0,
            supports_async_page_flip: false,
            supports_fb_modifiers: false,
            prefer_shadow_buffer: false,
            object_store: Mutex::new(object_store),
        })
    }

    /// Declares the fixed dimensions supported by hardware cursor planes.
    ///
    /// The cursor dimensions must be nonzero and fit within the device's maximum framebuffer dimensions.
    pub fn set_cursor_size(mut self, cursor_size: DrmSize) -> Result<Self> {
        if !cursor_size.is_within(1..=self.max_fb_size.width(), 1..=self.max_fb_size.height()) {
            return_errno_with_message!(
                Errno::EINVAL,
                "the cursor size is empty or exceeds the maximum framebuffer size"
            );
        }

        self.cursor_size = Some(cursor_size);
        Ok(self)
    }

    /// Sets the driver's preferred bit depth for dumb buffers.
    ///
    /// This records the device policy used when choosing a basic scanout buffer format.
    pub fn set_preferred_dumb_buffer_depth(mut self, depth: u32) -> Self {
        self.preferred_dumb_buffer_depth = depth;
        self
    }

    /// Advertises support for asynchronous page flips.
    ///
    /// Userspace can discover this capability through the DRM capability query interface.
    pub fn set_async_page_flip(mut self) -> Self {
        self.supports_async_page_flip = true;
        self
    }

    /// Advertises support for framebuffer format modifiers.
    ///
    /// Userspace can then use modifier-aware framebuffer creation ioctls.
    pub fn set_fb_modifiers(mut self) -> Self {
        self.supports_fb_modifiers = true;
        self
    }

    /// Marks that scanout should prefer a CPU-accessible shadow buffer.
    ///
    /// This is appropriate when rendering directly into the scanout buffer
    /// is inefficient or unsupported by the device.
    pub fn set_shadow_buffer(mut self) -> Self {
        self.prefer_shadow_buffer = true;
        self
    }

    pub(crate) fn min_fb_size(&self) -> DrmSize {
        self.min_fb_size
    }

    pub(crate) fn max_fb_size(&self) -> DrmSize {
        self.max_fb_size
    }

    pub(crate) fn cursor_size(&self) -> Option<DrmSize> {
        self.cursor_size
    }

    pub(crate) fn preferred_dumb_buffer_depth(&self) -> u32 {
        self.preferred_dumb_buffer_depth
    }

    pub(crate) fn supports_async_page_flip(&self) -> bool {
        self.supports_async_page_flip
    }

    /// Publishes the mode that is already active on the boot display.
    ///
    /// Firmware and bootloaders leave the display scanning out before the DRM
    /// driver is registered. Userspace compositors rely on the initial CRTC,
    /// encoder, and connector state to know that an output is already enabled;
    /// otherwise they may try to enable it without a scanout buffer.
    pub fn set_initial_enabled_mode(
        &self,
        crtc_index: KmsObjectIndex,
        encoder_index: KmsObjectIndex,
        connector_index: KmsObjectIndex,
        display_mode: DrmDisplayMode,
    ) -> Result<()> {
        let object_store = self.object_store.lock();

        let crtc = object_store
            .crtcs()
            .find(|crtc| crtc.index() == crtc_index)
            .ok_or(Errno::ENOENT)?;
        let encoder = object_store
            .encoders()
            .find(|encoder| encoder.index() == encoder_index)
            .ok_or(Errno::ENOENT)?;
        let connector = object_store
            .connectors()
            .find(|connector| connector.index() == connector_index)
            .ok_or(Errno::ENOENT)?;

        crtc.update_state(DrmCrtcState::new(display_mode));
        if let Some(primary_plane) = crtc.primary_plane().upgrade() {
            // The boot framebuffer is not a GEM object managed by this DRM
            // device, so there is no framebuffer ID to expose yet.
            primary_plane.update_state(DrmPlaneState::default());
        }

        encoder.set_current_crtc(Arc::downgrade(crtc));
        connector.update_state(DrmConnectorState::new(Arc::downgrade(encoder)));

        Ok(())
    }

    pub(crate) fn supports_fb_modifiers(&self) -> bool {
        self.supports_fb_modifiers
    }

    pub(crate) fn prefer_shadow_buffer(&self) -> bool {
        self.prefer_shadow_buffer
    }

    pub(crate) fn object_store(&self) -> &Mutex<DrmKmsObjectStore> {
        &self.object_store
    }
}
