//! Window-logical-pixel geometry matching GPUI's centered ObjectFit::Contain.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ContentRect {
    pub left: f64,
    pub top: f64,
    pub width: f64,
    pub height: f64,
}
impl ContentRect {
    pub fn contain(self, image_width: u32, image_height: u32) -> Option<Self> {
        if ![self.left, self.top, self.width, self.height]
            .iter()
            .all(|v| v.is_finite())
            || self.width <= 0.0
            || self.height <= 0.0
            || image_width == 0
            || image_height == 0
        {
            return None;
        }
        let scale =
            (self.width / f64::from(image_width)).min(self.height / f64::from(image_height));
        let width = f64::from(image_width) * scale;
        let height = f64::from(image_height) * scale;
        Some(Self {
            left: self.left + (self.width - width) / 2.0,
            top: self.top + (self.height - height) / 2.0,
            width,
            height,
        })
    }
    pub fn contains(self, x: f64, y: f64) -> bool {
        x.is_finite()
            && y.is_finite()
            && x >= self.left
            && y >= self.top
            && x <= self.left + self.width
            && y <= self.top + self.height
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn portrait_content_excludes_side_letterbox() {
        let rect = ContentRect {
            left: 10.0,
            top: 20.0,
            width: 800.0,
            height: 400.0,
        }
        .contain(100, 200)
        .unwrap();
        assert_eq!(
            rect,
            ContentRect {
                left: 310.0,
                top: 20.0,
                width: 200.0,
                height: 400.0
            }
        );
        assert!(!rect.contains(100.0, 30.0));
        assert!(rect.contains(410.0, 220.0));
    }
    #[test]
    fn landscape_content_excludes_top_letterbox_and_handles_scale() {
        let rect = ContentRect {
            left: 0.0,
            top: 0.0,
            width: 400.0,
            height: 800.0,
        }
        .contain(200, 100)
        .unwrap();
        assert_eq!(
            rect,
            ContentRect {
                left: 0.0,
                top: 300.0,
                width: 400.0,
                height: 200.0
            }
        );
    }
    #[test]
    fn invalid_geometry_cannot_enable_input() {
        assert!(
            ContentRect {
                left: 0.0,
                top: 0.0,
                width: 0.0,
                height: 1.0
            }
            .contain(1, 1)
            .is_none()
        );
        assert!(
            ContentRect {
                left: f64::NAN,
                top: 0.0,
                width: 1.0,
                height: 1.0
            }
            .contain(1, 1)
            .is_none()
        );
    }
}
