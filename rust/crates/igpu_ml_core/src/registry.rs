use crate::{ActionScores, Experience, FeatureVector, IgpuMlError};
use parking_lot::RwLock;
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Clone, Default)]
pub struct ModelRegistry {
    models: Arc<RwLock<HashMap<String, Box<dyn crate::model_trait::Model>>>>,
}

impl ModelRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register(&self, model: Box<dyn crate::model_trait::Model>) -> std::result::Result<(), IgpuMlError> {
        let id = model.id().to_string();
        let mut models = self.models.write();
        if models.contains_key(&id) {
            return Err(IgpuMlError::ModelAlreadyRegistered(id));
        }
        models.insert(id, model);
        Ok(())
    }

    pub fn infer(&self, id: &str, features: &FeatureVector) -> std::result::Result<ActionScores, IgpuMlError> {
        let models = self.models.read();
        let model = models
            .get(id)
            .ok_or_else(|| IgpuMlError::ModelNotFound(id.to_string()))?;
        Ok(model.infer(features))
    }

    pub fn observe(&self, id: &str, experience: &Experience) -> std::result::Result<(), IgpuMlError> {
        let mut models = self.models.write();
        let model = models
            .get_mut(id)
            .ok_or_else(|| IgpuMlError::ModelNotFound(id.to_string()))?;
        model.update(experience).map(|_| ())
    }

    pub fn list(&self) -> Vec<String> {
        let models = self.models.read();
        models.keys().cloned().collect()
    }
}
