--TEST--
Bug #61362 (Exception::getTraceAsString & __toString are not able to handle unicode) 
--FILE--
<?php
function test($arg){
	throw new Exception();
}

try {
	test('съешьещеэтихмягкихфранцузскихбулок');
}
catch(Exception $e) {
	echo $e->getTraceAsString();
	echo (string)$e;
}
?>
--EXPECTF--
#0 %sbug61362.php(%d): test('съешьеще...')
#1 {main}exception 'Exception' in %sbug61362.php:%d
Stack trace:
#0 %sbug61362.php(%d): test('съешьеще...')
#1 {main}
