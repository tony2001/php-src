--TEST--
stream_get_meta_data() basic functionality
--FILE--
<?php

$fp = fopen(__FILE__, "r");

var_dump(stream_get_meta_data($fp));

fclose($fp);

?>
--EXPECTF--
array(10) {
  ["wrapper_type"]=>
  string(9) "plainfile"
  ["stream_type"]=>
  string(5) "STDIO"
  ["fd"]=>
  int(%d)
  ["mode"]=>
  string(1) "r"
  ["unread_bytes"]=>
  int(0)
  ["seekable"]=>
  bool(true)
  ["uri"]=>
  string(%i) "%sstream_get_meta_data_file_basic.php"
  ["timed_out"]=>
  bool(false)
  ["blocked"]=>
  bool(true)
  ["eof"]=>
  bool(false)
}
