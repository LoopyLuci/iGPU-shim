use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct ModelExplanation {
    pub feature_importances: [f32; 8],
    pub predicted_scores: [f32; 3],
    pub confidence: f32,
    pub model_version: String,
    pub notes: String,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct UpdateResult {
    pub loss: f32,
    pub gradient_norm: f32,
    pub learning_rate: f32,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct BatchResult {
    pub avg_loss: f32,
    pub samples: usize,
    pub duration_us: u64,
}

pub trait Model: Send + Sync {
    fn id(&self) -> &str;
    fn version(&self) -> &str;

    fn infer(&self, features: &crate::FeatureVector) -> crate::ActionScores;
    fn update(&mut self, experience: &crate::Experience) -> std::result::Result<UpdateResult, crate::IgpuMlError>;
    fn batch_update(&mut self, batch: &[crate::Experience]) -> std::result::Result<BatchResult, crate::IgpuMlError>;

    fn serialize(&self) -> Vec<u8>;
    fn deserialize(&mut self, bytes: &[u8]) -> std::result::Result<(), crate::IgpuMlError>;

    fn explain(&self, features: &crate::FeatureVector) -> ModelExplanation;

    fn checkpoint(&self) -> Vec<u8> {
        self.serialize()
    }

    fn restore(&mut self, bytes: &[u8]) -> std::result::Result<(), crate::IgpuMlError> {
        self.deserialize(bytes)
    }
}
