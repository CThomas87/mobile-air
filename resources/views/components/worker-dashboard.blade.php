{{--
    NativePHP Worker Dashboard Component

    A self-contained Blade component that displays real-time worker/supervisor
    status. Drop this into any Blade template:

        <x-nativephp-worker-dashboard />

    Polls /_native/api/worker/status every 5 seconds and displays:
    - Supervisor status (running/stopped/starting/stopping)
    - Active, pending, completed, and failed job counts
    - Scheduler status
    - Uptime
    - Configuration summary
    - Queue database stats

    Uses Alpine.js for reactivity (assumes Alpine is already loaded).
--}}

<div x-data="workerDashboard()" x-init="init()" class="nativephp-worker-dashboard">
    <style>
        .nativephp-worker-dashboard {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            max-width: 600px;
            margin: 0 auto;
            padding: 16px;
        }

        .nwd-header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 16px;
        }

        .nwd-header h3 {
            margin: 0;
            font-size: 18px;
            font-weight: 600;
        }

        .nwd-status-badge {
            display: inline-block;
            padding: 4px 12px;
            border-radius: 12px;
            font-size: 12px;
            font-weight: 600;
            text-transform: uppercase;
        }

        .nwd-status-running {
            background: #d4edda;
            color: #155724;
        }

        .nwd-status-stopped {
            background: #f8d7da;
            color: #721c24;
        }

        .nwd-status-starting {
            background: #fff3cd;
            color: #856404;
        }

        .nwd-status-stopping {
            background: #fff3cd;
            color: #856404;
        }

        .nwd-status-unavailable {
            background: #e2e3e5;
            color: #383d41;
        }

        .nwd-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 12px;
            margin-bottom: 16px;
        }

        .nwd-stat {
            background: #f8f9fa;
            border-radius: 8px;
            padding: 12px;
            text-align: center;
        }

        .nwd-stat-value {
            font-size: 24px;
            font-weight: 700;
            color: #212529;
        }

        .nwd-stat-label {
            font-size: 12px;
            color: #6c757d;
            margin-top: 4px;
        }

        .nwd-config {
            background: #f8f9fa;
            border-radius: 8px;
            padding: 12px;
            font-size: 13px;
        }

        .nwd-config-row {
            display: flex;
            justify-content: space-between;
            padding: 4px 0;
            border-bottom: 1px solid #e9ecef;
        }

        .nwd-config-row:last-child {
            border-bottom: none;
        }

        .nwd-config-key {
            color: #6c757d;
        }

        .nwd-config-val {
            font-weight: 500;
            color: #212529;
        }

        .nwd-error {
            color: #dc3545;
            font-size: 13px;
            margin-top: 8px;
        }
    </style>

    <div class="nwd-header">
        <h3>Worker Dashboard</h3>
        <span class="nwd-status-badge" :class="'nwd-status-' + (data.status || 'unavailable')"
            x-text="data.status || 'unavailable'"></span>
    </div>

    <div class="nwd-grid">
        <div class="nwd-stat">
            <div class="nwd-stat-value" x-text="data.activeJobs ?? '-'"></div>
            <div class="nwd-stat-label">Active Jobs</div>
        </div>
        <div class="nwd-stat">
            <div class="nwd-stat-value" x-text="data.pendingJobs ?? '-'"></div>
            <div class="nwd-stat-label">Pending Jobs</div>
        </div>
        <div class="nwd-stat">
            <div class="nwd-stat-value" x-text="data.completedJobs ?? '-'"></div>
            <div class="nwd-stat-label">Completed</div>
        </div>
        <div class="nwd-stat">
            <div class="nwd-stat-value" x-text="data.failedJobs ?? '-'"></div>
            <div class="nwd-stat-label">Failed</div>
        </div>
    </div>

    <div class="nwd-grid">
        <div class="nwd-stat">
            <div class="nwd-stat-value" x-text="formatUptime(data.uptimeSeconds)"></div>
            <div class="nwd-stat-label">Uptime</div>
        </div>
        <div class="nwd-stat">
            <div class="nwd-stat-value" x-text="data.schedulerRunning ? 'Active' : 'Idle'"></div>
            <div class="nwd-stat-label">Scheduler</div>
        </div>
    </div>

    <template x-if="data.queueStats">
        <div class="nwd-grid">
            <div class="nwd-stat">
                <div class="nwd-stat-value" x-text="data.queueStats?.pendingInDatabase ?? '-'"></div>
                <div class="nwd-stat-label">DB Queue Pending</div>
            </div>
            <div class="nwd-stat">
                <div class="nwd-stat-value" x-text="data.queueStats?.failedInDatabase ?? '-'"></div>
                <div class="nwd-stat-label">DB Queue Failed</div>
            </div>
        </div>
    </template>

    <template x-if="data.config">
        <div class="nwd-config">
            <div class="nwd-config-row">
                <span class="nwd-config-key">Platform</span>
                <span class="nwd-config-val" x-text="data.config?.platform"></span>
            </div>
            <div class="nwd-config-row">
                <span class="nwd-config-key">Connection</span>
                <span class="nwd-config-val" x-text="data.config?.connection"></span>
            </div>
            <div class="nwd-config-row">
                <span class="nwd-config-key">Queues</span>
                <span class="nwd-config-val" x-text="data.config?.queues"></span>
            </div>
            <div class="nwd-config-row">
                <span class="nwd-config-key">Workers</span>
                <span class="nwd-config-val" x-text="data.config?.workerCount"></span>
            </div>
            <div class="nwd-config-row">
                <span class="nwd-config-key">Memory Limit</span>
                <span class="nwd-config-val" x-text="data.config?.memoryLimit"></span>
            </div>
            <div class="nwd-config-row">
                <span class="nwd-config-key">Circuit Breaker</span>
                <span class="nwd-config-val"
                    x-text="(data.config?.circuitBreakerThreshold || '-') + ' / ' + (data.config?.circuitBreakerBackoff || '-') + 's'"></span>
            </div>
        </div>
    </template>

    <div class="nwd-error" x-show="error" x-text="error"></div>
</div>

<script>
    function workerDashboard() {
        return {
            data: {},
            error: null,
            interval: null,

            init() {
                this.fetch();
                this.interval = setInterval(() => this.fetch(), 5000);
            },

            async fetch() {
                try {
                    const res = await window.fetch('/_native/api/worker/status');
                    if (!res.ok) throw new Error('HTTP ' + res.status);
                    this.data = await res.json();
                    this.error = null;
                } catch (e) {
                    this.error = 'Failed to load worker status: ' + e.message;
                }
            },

            formatUptime(seconds) {
                if (!seconds || seconds <= 0) return '-';
                const h = Math.floor(seconds / 3600);
                const m = Math.floor((seconds % 3600) / 60);
                const s = seconds % 60;
                if (h > 0) return h + 'h ' + m + 'm';
                if (m > 0) return m + 'm ' + s + 's';
                return s + 's';
            },

            destroy() {
                if (this.interval) clearInterval(this.interval);
            }
        };
    }
</script>
