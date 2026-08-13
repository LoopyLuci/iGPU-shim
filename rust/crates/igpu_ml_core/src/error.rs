use thiserror::Error;

#[derive(Error, Debug)]
pub enum IgpuMlError {
    #[error("model not found: {0}")]
    ModelNotFound(String),

    #[error("model already registered: {0}")]
    ModelAlreadyRegistered(String),

    #[error("invalid feature vector: {0}")]
    InvalidFeatureVector(String),

    #[error("serialization error: {0}")]
    SerializationError(String),

    #[error("deserialization error: {0}")]
    DeserializationError(String),

    #[error("replay buffer overflow")]
    ReplayBufferOverflow,

    #[error("replay buffer underflow")]
    ReplayBufferUnderflow,

    #[error("invalid action: {0}")]
    InvalidAction(u32),

    #[error("training error: {0}")]
    TrainingError(String),

    #[error("checkpoint error: {0}")]
    CheckpointError(String),

    #[error("FFI error: {0}")]
    FfiError(String),

    #[error("unknown error code: {0}")]
    UnknownError(i32),
}

pub type Result<T> = std::result::Result<T, IgpuMlError>;
