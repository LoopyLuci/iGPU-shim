use std::time::Duration;
use tokio::time::timeout;

#[tokio::test]
async fn dashboard_http_endpoints_ok() {
    let port = 18765u16;
    let server = igpu_ml_dashboard::run_with_port(port);
    let handle = tokio::spawn(async move { server.await });

    let base = format!("http://127.0.0.1:{}", port);
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(5))
        .build()
        .expect("build client");

    // Readiness check
    let ready = timeout(Duration::from_secs(10), async {
        loop {
            if let Ok(r) = client.get(&format!("{}/api/health", base)).send().await {
                if r.status().is_success() {
                    break;
                }
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    })
    .await;

    assert!(ready.is_ok(), "server did not become ready");

    let r = client.get(&format!("{}/api/health", base)).send().await.expect("health");
    assert!(r.status().is_success());

    let r = client.get(&format!("{}/api/report", base)).send().await.expect("report");
    assert!(r.status().is_success());

    let payload = serde_json::json!({
        "shader_complexity_norm": 0.1,
        "vertex_count_log_norm": 0.2,
        "draw_call_rate_norm": 0.3,
        "cache_hit_rate": 0.4,
        "dvfs_headroom": 0.5,
        "thermal_headroom": 0.6,
        "predictor_accuracy": 0.7,
        "is_compute_dispatch": 0.0
    });
    let r = client
        .post(&format!("{}/api/ml/infer", base))
        .json(&payload)
        .send()
        .await
        .expect("infer");
    assert!(r.status().is_success());

    handle.abort();
    let _ = timeout(Duration::from_secs(2), handle).await;
}
