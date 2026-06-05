use std::collections::VecDeque;

/// CHEEP SYNC — Standalone clock synchronization.
///
/// Estimates the linear relationship between two clocks so timestamps from a
/// beacon clock can be converted into the receiver clock's timeline.
///
/// Model:
///     Tr ≈ alpha + beta * tb
///
/// where:
/// - `tb` is beacon time in nanoseconds
/// - `Tr` is receiver time in nanoseconds
/// - `alpha` is offset in nanoseconds
/// - `beta` is skew, or the receiver/beacon clock-rate ratio
#[derive(Debug, Clone)]
pub struct CheepSync {
    window_size: usize,
    window: VecDeque<Sample>,
    alpha: f64,
    beta: f64,
    rms_residual_ms: f64,
}

#[derive(Debug, Clone, Copy)]
struct Sample {
    tb_ns: f64,
    tr_ns: f64,
}

impl CheepSync {
    pub const DEFAULT_WINDOW_SIZE: usize = 50;

    /// Create a new synchronizer with the default window size.
    pub fn new() -> Self {
        Self::with_window_size(Self::DEFAULT_WINDOW_SIZE)
    }

    /// Create a new synchronizer with a custom sliding-window size.
    pub fn with_window_size(window_size: usize) -> Self {
        Self {
            window_size,
            window: VecDeque::new(),
            alpha: 0.0,
            beta: 1.0,
            rms_residual_ms: 0.0,
        }
    }

    /// Current offset alpha in nanoseconds.
    /// When beacon time is 0, receiver time is approximately alpha.
    pub fn alpha(&self) -> f64 {
        self.alpha
    }

    /// Current skew beta, dimensionless.
    /// beta is the ratio of receiver clock rate to beacon clock rate.
    pub fn beta(&self) -> f64 {
        self.beta
    }

    /// Root-mean-square residual error in milliseconds.
    pub fn rms_residual_ms(&self) -> f64 {
        self.rms_residual_ms
    }

    /// Number of samples currently in the sliding window.
    pub fn sample_count(&self) -> usize {
        self.window.len()
    }

    /// Add one `(beacon_time_us, receiver_time_ns)` sample and recompute
    /// `alpha`, `beta`, and `rms_residual_ms` using least-squares regression.
    ///
    /// At least two samples are required before a fit can be computed.
    pub fn add_sample(&mut self, beacon_time_us: i64, receiver_time_ns: i64) {
        let tb_ns = beacon_time_us as f64 * 1000.0;
        let tr_ns = receiver_time_ns as f64;

        self.window.push_back(Sample { tb_ns, tr_ns });
        while self.window.len() > self.window_size {
            self.window.pop_front();
        }

        if self.window.len() < 2 {
            return;
        }

        let n = self.window.len() as f64;

        let (tb_sum, tr_sum) = self
            .window
            .iter()
            .fold((0.0, 0.0), |(tb_acc, tr_acc), sample| {
                (tb_acc + sample.tb_ns, tr_acc + sample.tr_ns)
            });

        let tb_mean = tb_sum / n;
        let tr_mean = tr_sum / n;

        let (cov, var_tb) = self.window.iter().fold((0.0, 0.0), |(cov_acc, var_acc), sample| {
            let x = sample.tb_ns - tb_mean;
            let y = sample.tr_ns - tr_mean;
            (cov_acc + x * y, var_acc + x * x)
        });

        if var_tb == 0.0 {
            return;
        }

        self.beta = cov / var_tb;
        self.alpha = tr_mean - self.beta * tb_mean;

        let rss = self.window.iter().fold(0.0, |acc, sample| {
            let predicted = self.alpha + self.beta * sample.tb_ns;
            let residual = sample.tr_ns - predicted;
            acc + residual * residual
        });

        let rms_ns = (rss / n).sqrt();
        self.rms_residual_ms = rms_ns / 1_000_000.0;
    }

    /// Convert a beacon timestamp in microseconds into the receiver timeline in nanoseconds.
    pub fn map_beacon_to_receiver_ns(&self, beacon_time_us: i64) -> i64 {
        let tb_ns = beacon_time_us as f64 * 1000.0;
        (self.alpha + self.beta * tb_ns) as i64
    }

    /// Sync error for one sample: absolute actual-vs-predicted error in milliseconds.
    pub fn residual_ms(&self, beacon_time_us: i64, receiver_time_ns: i64) -> f64 {
        let tb_ns = beacon_time_us as f64 * 1000.0;
        let predicted = self.alpha + self.beta * tb_ns;
        (receiver_time_ns as f64 - predicted).abs() / 1_000_000.0
    }

    /// Immutable snapshot of `(alpha, beta)` so other code can convert timestamps
    /// without holding a reference to `CheepSync`.
    pub fn get_fit(&self) -> SyncFit {
        SyncFit {
            alpha: self.alpha,
            beta: self.beta,
        }
    }

    /// Clear the window and reset alpha=0, beta=1, rms_residual_ms=0.
    /// Call this when starting a new connection or session.
    pub fn reset(&mut self) {
        self.window.clear();
        self.alpha = 0.0;
        self.beta = 1.0;
        self.rms_residual_ms = 0.0;
    }
}

impl Default for CheepSync {
    fn default() -> Self {
        Self::new()
    }
}

/// Immutable copy of `(alpha, beta)`.
/// Use this to convert timestamps in another module without depending on `CheepSync`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SyncFit {
    pub alpha: f64,
    pub beta: f64,
}

impl SyncFit {
    /// Convert beacon microseconds to receiver nanoseconds.
    pub fn map_beacon_to_receiver_ns(&self, beacon_time_us: i64) -> i64 {
        let tb_ns = beacon_time_us as f64 * 1000.0;
        (self.alpha + self.beta * tb_ns) as i64
    }

    /// Convert beacon microseconds to receiver milliseconds.
    pub fn map_beacon_to_receiver_ms(&self, beacon_time_us: i64) -> f64 {
        self.map_beacon_to_receiver_ns(beacon_time_us) as f64 / 1_000_000.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn computes_linear_fit() {
        let mut sync = CheepSync::new();

        // receiver_ns = 10_000 + 2 * beacon_ns
        sync.add_sample(1, 12_000);
        sync.add_sample(2, 14_000);
        sync.add_sample(3, 16_000);

        assert!((sync.alpha() - 10_000.0).abs() < 1e-6);
        assert!((sync.beta() - 2.0).abs() < 1e-12);
        assert_eq!(sync.map_beacon_to_receiver_ns(4), 18_000);
        assert!(sync.rms_residual_ms() < 1e-12);
    }

    #[test]
    fn reset_restores_defaults() {
        let mut sync = CheepSync::new();
        sync.add_sample(1, 12_000);
        sync.add_sample(2, 14_000);
        sync.reset();

        assert_eq!(sync.sample_count(), 0);
        assert_eq!(sync.alpha(), 0.0);
        assert_eq!(sync.beta(), 1.0);
        assert_eq!(sync.rms_residual_ms(), 0.0);
    }
}