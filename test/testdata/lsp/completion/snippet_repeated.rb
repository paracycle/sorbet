# typed: true
class A
  extend T::Sig

  sig {params(x: T.untyped).void}
  def method_using_kwarg(**x)
  end

  sig {params(x: T.untyped).void}
  def method_using_rest_arg(*x)
  end

  sig {params(x: Integer, y: T.untyped).void}
  def positional_then_kwarg(x, *y)
  end

  sig {params(x: Integer, y: T.untyped).void}
  def positional_then_rest_arg(x, *y)
  end
end

A.new.method_using_kwar # error: does not exist
#                      ^ apply-completion: [A] item: 0

A.new.method_using_rest_ar # error: does not exist
#                         ^ apply-completion: [B] item: 0

A.new.positional_then_kwar # error: does not exist
#                         ^ apply-completion: [C] item: 0

A.new.positional_then_rest_ar # error: does not exist
#                            ^ apply-completion: [D] item: 0
