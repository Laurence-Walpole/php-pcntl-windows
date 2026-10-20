<?php
// Prints "version|bits|ts|major.minor|build-id" for build.ps1 / install.ps1
$build = '';
ob_start();
phpinfo(INFO_GENERAL);
$info = ob_get_clean();
if (preg_match('/PHP Extension Build => (\S+)/', $info, $m)) {
    $build = $m[1];
}
echo PHP_VERSION, '|', PHP_INT_SIZE * 8, '|', PHP_ZTS ? 'ts' : 'nts', '|', PHP_MAJOR_VERSION, '.', PHP_MINOR_VERSION, '|', $build;
