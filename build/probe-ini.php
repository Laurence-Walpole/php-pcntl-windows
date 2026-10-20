<?php
// Prints "extension_dir|loaded php.ini|pcntl loaded (1/0)" using the real configuration (no -n), for install.ps1
echo ini_get('extension_dir'), '|', (string) php_ini_loaded_file(), '|', extension_loaded('pcntl') ? '1' : '0';
