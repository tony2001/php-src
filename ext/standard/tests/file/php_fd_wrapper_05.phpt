--TEST--
php://fd wrapper: raw option
--FILE--
<?php
$f = fopen("php://fd/2", "wb");
$f_meta = stream_get_meta_data($f);
fwrite($f, "fd is duped:" . var_export($f_meta['fd'] != 2, true). "\n");

$g = fopen("php://fd/2/raw", "wb");
$g_meta = stream_get_meta_data($g);
fwrite($g, "fd is same:" . var_export($g_meta['fd'] == 2, true) . "\n");

echo "Done.\n";
--EXPECT--
fd is duped:true
fd is same:true
Done.
