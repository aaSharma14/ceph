import { Directive, Input } from '@angular/core';

@Directive({
  selector: '[cdScope]'
})
export class PermissionScopeDirective {
  @Input() cdScope: any;

  constructor() {}
}
