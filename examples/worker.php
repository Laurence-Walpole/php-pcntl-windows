<?php
/**
 * A long-running worker with graceful shutdown and a per-job timeout, the way
 * Laravel's queue:work / Symfony Console signal handling do it. Runs unchanged
 * on Linux and, with pcntl for Windows, on Windows.
 *
 *   php examples\worker.php
 *
 * Then press Ctrl+C, or from another terminal:
 *   php -r "posix_kill(<pid>, SIGTERM);"
 */
declare(strict_types=1);

pcntl_async_signals(true);

$shouldStop = false;
$stop = function (int $signo) use (&$shouldStop): void {
    echo PHP_EOL, "Received ", ['SIGINT', 'SIGTERM', 'SIGQUIT', 'SIGHUP'][array_search($signo, [SIGINT, SIGTERM, SIGQUIT, SIGHUP], true)], ", finishing current job then stopping...", PHP_EOL;
    $shouldStop = true;
};
pcntl_signal(SIGINT, $stop);
pcntl_signal(SIGTERM, $stop);
pcntl_signal(SIGQUIT, $stop);   // Ctrl+Break on Windows
pcntl_signal(SIGHUP, $stop);    // console window closed on Windows

// job timeout: SIGALRM fires if a job runs too long
pcntl_signal(SIGALRM, function (): void {
    echo PHP_EOL, "Job exceeded its timeout, killing worker so the supervisor can restart it", PHP_EOL;
    posix_kill(getmypid(), SIGKILL);
});

echo "Worker pid ", getmypid(), " started. Ctrl+C to stop, or: php -r \"posix_kill(", getmypid(), ", SIGTERM);\"", PHP_EOL;

$job = 0;
while (!$shouldStop) {
    $job++;
    pcntl_alarm(5);                 // this job may take at most 5 seconds
    echo "job $job ";
    usleep(random_int(200_000, 900_000)); // pretend to work
    pcntl_alarm(0);                 // done in time, cancel the timeout
}

echo "Stopped cleanly after $job jobs.", PHP_EOL;
