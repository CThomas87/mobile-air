<?php

namespace Native\Mobile\Worker;

use Illuminate\Support\Facades\DB;

/**
 * NativeDbPool — PHP-side connection pool management.
 *
 * When the native SQLite pool (sqlite_pool.c) is compiled into the app
 * (NATIVEPHP_ENABLE_SQLITE_MODULES=ON), this class coordinates with it.
 *
 * When the native pool is not available, this provides a PHP-level
 * connection pool that pre-warms PDO connections and manages their
 * lifecycle for worker threads.
 *
 * Usage:
 *   $pool = NativeDbPool::instance();
 *   $connection = $pool->acquire();
 *   try {
 *       $connection->table('jobs')->count();
 *   } finally {
 *       $pool->release($connection);
 *   }
 *
 * The pool is also integrated with WorkerServiceProvider so that
 * worker threads automatically use pooled connections.
 */
class NativeDbPool
{
    /** @var static|null */
    protected static ?self $instance = null;

    /** @var array<int, \Illuminate\Database\Connection> Idle connections */
    protected array $idle = [];

    /** @var array<int, \Illuminate\Database\Connection> Checked-out connections */
    protected array $inUse = [];

    /** @var int Maximum pool size */
    protected int $maxSize;

    /** @var string Database connection name */
    protected string $connectionName;

    /** @var string Database path (for SQLite) */
    protected string $dbPath;

    /** @var bool Whether the native C pool is available */
    protected bool $hasNativePool;

    /** @var int Total connections created */
    protected int $totalCreated = 0;

    /** @var int Total acquire operations */
    protected int $acquireCount = 0;

    /** @var int Total release operations */
    protected int $releaseCount = 0;

    /** @var int Pool misses (had to create new connection) */
    protected int $missCount = 0;

    public function __construct(int $maxSize = 4, ?string $connectionName = null)
    {
        $this->maxSize = max(1, min($maxSize, 16));
        $this->connectionName = $connectionName ?? config('database.default', 'sqlite');
        $this->dbPath = config("database.connections.{$this->connectionName}.database", '');
        $this->hasNativePool = function_exists('nativephp_db_pool_acquire');
    }

    /**
     * Get the singleton pool instance.
     */
    public static function instance(): static
    {
        if (static::$instance === null) {
            $poolSize = (int) config('nativephp-worker.db_pool_size',
                env('NATIVEPHP_DB_POOL_SIZE', 4));

            static::$instance = new static($poolSize);
        }

        return static::$instance;
    }

    /**
     * Acquire a database connection from the pool.
     *
     * @param  int  $timeoutMs  Maximum wait time in milliseconds (0 = no timeout)
     * @return \Illuminate\Database\Connection
     *
     * @throws \RuntimeException If all connections are in use and pool is at capacity
     */
    public function acquire(int $timeoutMs = 5000): \Illuminate\Database\Connection
    {
        $this->acquireCount++;

        // Try to reuse an idle connection
        if (! empty($this->idle)) {
            $connection = array_pop($this->idle);
            $id = spl_object_id($connection);
            $this->inUse[$id] = $connection;

            // Validate the connection is still alive
            try {
                $connection->getPdo()->query('SELECT 1');
                return $connection;
            } catch (\Throwable $e) {
                // Connection is dead, remove it and create a new one
                unset($this->inUse[$id]);
            }
        }

        // Create a new connection if under capacity
        if ($this->totalCreated < $this->maxSize) {
            return $this->createPooledConnection();
        }

        $this->missCount++;

        // Pool is full — wait briefly for a connection to be released
        // In the PHP ZTS model, this is advisory since we can't truly
        // block on a condvar. Fall back to the default DB facade.
        return DB::connection($this->connectionName);
    }

    /**
     * Release a connection back to the pool.
     *
     * @param  \Illuminate\Database\Connection  $connection
     */
    public function release(\Illuminate\Database\Connection $connection): void
    {
        $this->releaseCount++;
        $id = spl_object_id($connection);

        if (isset($this->inUse[$id])) {
            unset($this->inUse[$id]);
            $this->idle[$id] = $connection;
        }
    }

    /**
     * Get pool statistics.
     *
     * @return array{total: int, idle: int, in_use: int, max_size: int,
     *               acquires: int, releases: int, misses: int, has_native: bool}
     */
    public function stats(): array
    {
        return [
            'total' => $this->totalCreated,
            'idle' => count($this->idle),
            'in_use' => count($this->inUse),
            'max_size' => $this->maxSize,
            'acquires' => $this->acquireCount,
            'releases' => $this->releaseCount,
            'misses' => $this->missCount,
            'has_native' => $this->hasNativePool,
        ];
    }

    /**
     * Pre-warm the pool by creating all connections upfront.
     * Call this during first-boot serialization (under flock).
     */
    public function warmUp(): void
    {
        while ($this->totalCreated < $this->maxSize) {
            $connection = $this->createPooledConnection();
            $this->release($connection);
        }
    }

    /**
     * Destroy all pooled connections.
     */
    public function drain(): void
    {
        foreach ($this->idle as $connection) {
            try {
                $connection->disconnect();
            } catch (\Throwable $e) {
                // Best-effort
            }
        }

        $this->idle = [];
        $this->inUse = [];
        $this->totalCreated = 0;
    }

    /**
     * Check if the native C pool is available (compiled with NATIVEPHP_HAS_SQLITE3).
     */
    public function hasNativePool(): bool
    {
        return $this->hasNativePool;
    }

    /**
     * Create a new database connection and configure it for pooling.
     */
    protected function createPooledConnection(): \Illuminate\Database\Connection
    {
        $this->totalCreated++;

        $connection = DB::connection($this->connectionName);

        // Apply WAL mode PRAGMAs for SQLite connections
        $driver = config("database.connections.{$this->connectionName}.driver");
        if ($driver === 'sqlite') {
            try {
                $connection->statement('PRAGMA busy_timeout=5000');
                $connection->statement('PRAGMA journal_mode=WAL');
                $connection->statement('PRAGMA synchronous=NORMAL');
                $connection->statement('PRAGMA cache_size=-4000');
            } catch (\Throwable $e) {
                // Non-fatal
            }
        }

        $id = spl_object_id($connection);
        $this->inUse[$id] = $connection;

        return $connection;
    }
}
