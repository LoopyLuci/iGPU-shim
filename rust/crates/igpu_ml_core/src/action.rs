use serde::{Deserialize, Serialize};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum Backend {
    #[default]
    Jit = 0,
    Hai = 1,
    Oracle = 2,
}

impl Backend {
    pub const COUNT: usize = 3;

    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Backend::Jit),
            1 => Some(Backend::Hai),
            2 => Some(Backend::Oracle),
            _ => None,
        }
    }

    pub fn to_u8(self) -> u8 {
        self as u8
    }

    pub fn name(&self) -> &'static str {
        match self {
            Backend::Jit => "JIT",
            Backend::Hai => "HAI",
            Backend::Oracle => "Oracle",
        }
    }
}

#[repr(C)]
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct ActionScores {
    pub scores: [f32; 3],
    pub chosen: u32,
    pub confidence: f32,
    pub epsilon_greedy: bool,
}

impl ActionScores {
    pub fn choose(&self) -> Backend {
        let mut best_idx = 0;
        let mut best_val = self.scores[0];
        for i in 1..3 {
            if self.scores[i] > best_val {
                best_val = self.scores[i];
                best_idx = i;
            }
        }
        Backend::from_u8(best_idx as u8).unwrap_or(Backend::Oracle)
    }

    pub fn confidence(&self) -> f32 {
        let max = self.scores.iter().copied().reduce(f32::max).unwrap_or(0.0);
        let mean = self.scores.iter().copied().sum::<f32>() / 3.0;
        (max - mean).clamp(0.0, 1.0)
    }
}
