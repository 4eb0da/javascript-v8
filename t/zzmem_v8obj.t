#!/usr/bin/perl

use Test::More;
use JavaScript::V8;
use Data::Dumper;

use utf8;
use strict;
use warnings;

sub test {
    $_;
}

my $context;

$context = JavaScript::V8::Context->new;
$context->eval('var abe = function(){}');
for (my $i = 0; $i < 1000; $i++) {
    test($context->eval('abe'));
}

ok('Done');

done_testing;
