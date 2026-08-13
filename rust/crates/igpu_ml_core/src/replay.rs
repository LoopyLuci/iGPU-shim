use crate::Experience;
use parking_lot::RwLock;
use std::collections::VecDeque;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub struct ReplayBuffer {
    buffer: Arc<RwLock<VecDeque<Experience>>>,
    capacity: usize,
}

impl ReplayBuffer {
    pub fn new(capacity: usize) -> Self {
        Self {
            buffer: Arc::new(RwLock::new(VecDeque::with_capacity(capacity))),
            capacity,
        }
    }

    pub fn push(&self, experience: Experience) {
        let mut buf = self.buffer.write();
        if buf.len() >= self.capacity {
            buf.pop_front();
        }
        buf.push_back(experience);
    }

    pub fn extend<I: IntoIterator<Item = Experience>>(&self, experiences: I) {
        let mut buf = self.buffer.write();
        for exp in experiences {
            if buf.len() >= self.capacity {
                buf.pop_front();
            }
            buf.push_back(exp);
        }
    }

    pub fn sample(&self, n: usize) -> Vec<Experience> {
        let buf = self.buffer.read();
        let len = buf.len();
        if len == 0 {
            return Vec::new();
        }
        let n = n.min(len);
        let mut samples = Vec::with_capacity(n);
        let step = len as f32 / n as f32;
        for i in 0..n {
            let idx = ((i as f32 * step) as usize).min(len - 1);
            samples.push(buf[idx].clone());
        }
        samples
    }

    pub fn len(&self) -> usize {
        self.buffer.read().len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }

    pub fn clear(&self) {
        self.buffer.write().clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn push_and_sample() {
        let buf = ReplayBuffer::new(10);
        for i in 0..5 {
            buf.push(Experience::new(
                crate::FeatureVector::default(),
                crate::Backend::Jit as u32,
                i as f32 / 5.0,
            ));
        }
        assert_eq!(buf.len(), 5);
        let samples = buf.sample(3);
        assert_eq!(samples.len(), 3);
    }

    #[test]
    fn overflow_eviction() {
        let buf = ReplayBuffer::new(3);
        for i in 0..5 {
            buf.push(Experience::new(
                crate::FeatureVector::default(),
                crate::Backend::Jit as u32,
                i as f32,
            ));
        }
        assert_eq!(buf.len(), 3);
    }
}
